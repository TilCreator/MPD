/*
 * Copyright 2003-2016 The Music Player Daemon Project
 * http://www.musicpd.org
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License along
 * with this program; if not, write to the Free Software Foundation, Inc.,
 * 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA.
 */

#include "config.h"
#include "DecoderAPI.hxx"
#include "DecoderError.hxx"
#include "pcm/PcmConvert.hxx"
#include "AudioConfig.hxx"
#include "ReplayGainConfig.hxx"
#include "MusicChunk.hxx"
#include "MusicBuffer.hxx"
#include "MusicPipe.hxx"
#include "DecoderControl.hxx"
#include "Bridge.hxx"
#include "DetachedSong.hxx"
#include "input/InputStream.hxx"
#include "util/ConstBuffer.hxx"
#include "Log.hxx"

#include <assert.h>
#include <string.h>
#include <math.h>

void
DecoderBridge::Ready(const AudioFormat audio_format,
		     bool seekable, SignedSongTime duration)
{
	struct audio_format_string af_string;

	assert(dc.state == DecoderState::START);
	assert(dc.pipe != nullptr);
	assert(dc.pipe->IsEmpty());
	assert(convert == nullptr);
	assert(stream_tag == nullptr);
	assert(decoder_tag == nullptr);
	assert(!seeking);
	assert(audio_format.IsDefined());
	assert(audio_format.IsValid());

	dc.in_audio_format = audio_format;
	dc.out_audio_format = getOutputAudioFormat(audio_format);

	dc.seekable = seekable;
	dc.total_time = duration;

	FormatDebug(decoder_domain, "audio_format=%s, seekable=%s",
		    audio_format_to_string(dc.in_audio_format, &af_string),
		    seekable ? "true" : "false");

	if (dc.in_audio_format != dc.out_audio_format) {
		FormatDebug(decoder_domain, "converting to %s",
			    audio_format_to_string(dc.out_audio_format,
						   &af_string));

		convert = new PcmConvert();

		try {
			convert->Open(dc.in_audio_format,
					      dc.out_audio_format);
		} catch (...) {
			error = std::current_exception();
		}
	}

	const ScopeLock protect(dc.mutex);
	dc.state = DecoderState::DECODE;
	dc.client_cond.signal();
}

bool
DecoderBridge::PrepareInitialSeek()
{
	assert(dc.pipe != nullptr);

	if (dc.state != DecoderState::DECODE)
		/* wait until the decoder has finished initialisation
		   (reading file headers etc.) before emitting the
		   virtual "SEEK" command */
		return false;

	if (initial_seek_running)
		/* initial seek has already begun - override any other
		   command */
		return true;

	if (initial_seek_pending) {
		if (!dc.seekable) {
			/* seeking is not possible */
			initial_seek_pending = false;
			return false;
		}

		if (dc.command == DecoderCommand::NONE) {
			/* begin initial seek */

			initial_seek_pending = false;
			initial_seek_running = true;
			return true;
		}

		/* skip initial seek when there's another command
		   (e.g. STOP) */

		initial_seek_pending = false;
	}

	return false;
}

DecoderCommand
DecoderBridge::GetVirtualCommand()
{
	if (error)
		/* an error has occurred: stop the decoder plugin */
		return DecoderCommand::STOP;

	assert(dc.pipe != nullptr);

	if (PrepareInitialSeek())
		return DecoderCommand::SEEK;

	return dc.command;
}

DecoderCommand
DecoderBridge::LockGetVirtualCommand()
{
	const ScopeLock protect(dc.mutex);
	return GetVirtualCommand();
}

DecoderCommand
DecoderBridge::GetCommand()
{
	return LockGetVirtualCommand();
}

void
DecoderBridge::CommandFinished()
{
	const ScopeLock protect(dc.mutex);

	assert(dc.command != DecoderCommand::NONE || initial_seek_running);
	assert(dc.command != DecoderCommand::SEEK ||
	       initial_seek_running ||
	       dc.seek_error || seeking);
	assert(dc.pipe != nullptr);

	if (initial_seek_running) {
		assert(!seeking);
		assert(current_chunk == nullptr);
		assert(dc.pipe->IsEmpty());

		initial_seek_running = false;
		timestamp = dc.start_time.ToDoubleS();
		return;
	}

	if (seeking) {
		seeking = false;

		/* delete frames from the old song position */

		if (current_chunk != nullptr) {
			dc.buffer->Return(current_chunk);
			current_chunk = nullptr;
		}

		dc.pipe->Clear(*dc.buffer);

		timestamp = dc.seek_time.ToDoubleS();
	}

	dc.command = DecoderCommand::NONE;
	dc.client_cond.signal();
}

SongTime
DecoderBridge::GetSeekTime()
{
	assert(dc.pipe != nullptr);

	if (initial_seek_running)
		return dc.start_time;

	assert(dc.command == DecoderCommand::SEEK);

	seeking = true;

	return dc.seek_time;
}

uint64_t
DecoderBridge::GetSeekFrame()
{
	return GetSeekTime().ToScale<uint64_t>(dc.in_audio_format.sample_rate);
}

void
DecoderBridge::SeekError()
{
	assert(dc.pipe != nullptr);

	if (initial_seek_running) {
		/* d'oh, we can't seek to the sub-song start position,
		   what now? - no idea, ignoring the problem for now. */
		initial_seek_running = false;
		return;
	}

	assert(dc.command == DecoderCommand::SEEK);

	dc.seek_error = true;
	seeking = false;

	CommandFinished();
}

InputStreamPtr
DecoderBridge::OpenUri(const char *uri)
{
	assert(dc.state == DecoderState::START ||
	       dc.state == DecoderState::DECODE);

	Mutex &mutex = dc.mutex;
	Cond &cond = dc.cond;

	auto is = InputStream::Open(uri, mutex, cond);

	const ScopeLock lock(mutex);
	while (true) {
		is->Update();
		if (is->IsReady())
			return is;

		if (dc.command == DecoderCommand::STOP)
			throw StopDecoder();

		cond.wait(mutex);
	}
}

/**
 * Should be read operation be cancelled?  That is the case when the
 * player thread has sent a command such as "STOP".
 */
gcc_pure
static inline bool
decoder_check_cancel_read(const DecoderBridge *bridge)
{
	return bridge != nullptr && bridge->CheckCancelRead();
}

size_t
decoder_read(DecoderClient *client,
	     InputStream &is,
	     void *buffer, size_t length)
try {
	/* XXX don't allow decoder==nullptr */
	auto *bridge = (DecoderBridge *)client;

	assert(bridge == nullptr ||
	       bridge->dc.state == DecoderState::START ||
	       bridge->dc.state == DecoderState::DECODE);
	assert(buffer != nullptr);

	if (length == 0)
		return 0;

	ScopeLock lock(is.mutex);

	while (true) {
		if (decoder_check_cancel_read(bridge))
			return 0;

		if (is.IsAvailable())
			break;

		is.cond.wait(is.mutex);
	}

	size_t nbytes = is.Read(buffer, length);
	assert(nbytes > 0 || is.IsEOF());

	return nbytes;
} catch (const std::runtime_error &e) {
	auto *bridge = (DecoderBridge *)client;
	if (bridge != nullptr)
		bridge->error = std::current_exception();
	else
		LogError(e);
	return 0;
}

bool
decoder_read_full(DecoderClient *client, InputStream &is,
		  void *_buffer, size_t size)
{
	uint8_t *buffer = (uint8_t *)_buffer;

	while (size > 0) {
		size_t nbytes = decoder_read(client, is, buffer, size);
		if (nbytes == 0)
			return false;

		buffer += nbytes;
		size -= nbytes;
	}

	return true;
}

bool
decoder_skip(DecoderClient *client, InputStream &is, size_t size)
{
	while (size > 0) {
		char buffer[1024];
		size_t nbytes = decoder_read(client, is, buffer,
					     std::min(sizeof(buffer), size));
		if (nbytes == 0)
			return false;

		size -= nbytes;
	}

	return true;
}

void
DecoderBridge::SubmitTimestamp(double t)
{
	assert(t >= 0);

	timestamp = t;
}

DecoderCommand
DecoderBridge::DoSendTag(const Tag &tag)
{
	if (current_chunk != nullptr) {
		/* there is a partial chunk - flush it, we want the
		   tag in a new chunk */
		FlushChunk();
	}

	assert(current_chunk == nullptr);

	auto *chunk = GetChunk();
	if (chunk == nullptr) {
		assert(dc.command != DecoderCommand::NONE);
		return dc.command;
	}

	chunk->tag = new Tag(tag);
	return DecoderCommand::NONE;
}

bool
DecoderBridge::UpdateStreamTag(InputStream *is)
{
	auto *tag = is != nullptr
		? is->LockReadTag()
		: nullptr;
	if (tag == nullptr) {
		tag = song_tag;
		if (tag == nullptr)
			return false;

		/* no stream tag present - submit the song tag
		   instead */
	} else
		/* discard the song tag; we don't need it */
		delete song_tag;

	song_tag = nullptr;

	delete stream_tag;
	stream_tag = tag;
	return true;
}

DecoderCommand
DecoderBridge::SubmitData(InputStream *is,
			  const void *data, size_t length,
			  uint16_t kbit_rate)
{
	assert(dc.state == DecoderState::DECODE);
	assert(dc.pipe != nullptr);
	assert(length % dc.in_audio_format.GetFrameSize() == 0);

	DecoderCommand cmd = LockGetVirtualCommand();

	if (cmd == DecoderCommand::STOP || cmd == DecoderCommand::SEEK ||
	    length == 0)
		return cmd;

	assert(!initial_seek_pending);
	assert(!initial_seek_running);

	/* send stream tags */

	if (UpdateStreamTag(is)) {
		if (decoder_tag != nullptr) {
			/* merge with tag from decoder plugin */
			Tag *tag = Tag::Merge(*decoder_tag,
					      *stream_tag);
			cmd = DoSendTag(*tag);
			delete tag;
		} else
			/* send only the stream tag */
			cmd = DoSendTag(*stream_tag);

		if (cmd != DecoderCommand::NONE)
			return cmd;
	}

	if (convert != nullptr) {
		assert(dc.in_audio_format != dc.out_audio_format);

		try {
			auto result = convert->Convert({data, length});
			data = result.data;
			length = result.size;
		} catch (const std::runtime_error &e) {
			/* the PCM conversion has failed - stop
			   playback, since we have no better way to
			   bail out */
			error = std::current_exception();
			return DecoderCommand::STOP;
		}
	} else {
		assert(dc.in_audio_format == dc.out_audio_format);
	}

	while (length > 0) {
		bool full;

		auto *chunk = GetChunk();
		if (chunk == nullptr) {
			assert(dc.command != DecoderCommand::NONE);
			return dc.command;
		}

		const auto dest =
			chunk->Write(dc.out_audio_format,
				     SongTime::FromS(timestamp) -
				     dc.song->GetStartTime(),
				     kbit_rate);
		if (dest.IsEmpty()) {
			/* the chunk is full, flush it */
			FlushChunk();
			continue;
		}

		const size_t nbytes = std::min(dest.size, length);

		/* copy the buffer */

		memcpy(dest.data, data, nbytes);

		/* expand the music pipe chunk */

		full = chunk->Expand(dc.out_audio_format, nbytes);
		if (full) {
			/* the chunk is full, flush it */
			FlushChunk();
		}

		data = (const uint8_t *)data + nbytes;
		length -= nbytes;

		timestamp += (double)nbytes /
			dc.out_audio_format.GetTimeToSize();

		if (dc.end_time.IsPositive() &&
		    timestamp >= dc.end_time.ToDoubleS())
			/* the end of this range has been reached:
			   stop decoding */
			return DecoderCommand::STOP;
	}

	return DecoderCommand::NONE;
}

DecoderCommand
DecoderBridge::SubmitTag(InputStream *is, Tag &&tag)
{
	DecoderCommand cmd;

	assert(dc.state == DecoderState::DECODE);
	assert(dc.pipe != nullptr);

	/* save the tag */

	delete decoder_tag;
	decoder_tag = new Tag(std::move(tag));

	/* check for a new stream tag */

	UpdateStreamTag(is);

	/* check if we're seeking */

	if (PrepareInitialSeek())
		/* during initial seek, no music chunk must be created
		   until seeking is finished; skip the rest of the
		   function here */
		return DecoderCommand::SEEK;

	/* send tag to music pipe */

	if (stream_tag != nullptr) {
		/* merge with tag from input stream */
		Tag *merged;

		merged = Tag::Merge(*stream_tag, *decoder_tag);
		cmd = DoSendTag(*merged);
		delete merged;
	} else
		/* send only the decoder tag */
		cmd = DoSendTag(*decoder_tag);

	return cmd;
}

void
DecoderBridge::SubmitReplayGain(const ReplayGainInfo *new_replay_gain_info)
{
	if (new_replay_gain_info != nullptr) {
		static unsigned serial;
		if (++serial == 0)
			serial = 1;

		if (REPLAY_GAIN_OFF != replay_gain_mode) {
			ReplayGainMode rgm = replay_gain_mode;
			if (rgm != REPLAY_GAIN_ALBUM)
				rgm = REPLAY_GAIN_TRACK;

			const auto &tuple = new_replay_gain_info->tuples[rgm];
			const auto scale =
				tuple.CalculateScale(replay_gain_preamp,
						     replay_gain_missing_preamp,
						     replay_gain_limit);
			dc.replay_gain_db = 20.0 * log10f(scale);
		}

		replay_gain_info = *new_replay_gain_info;
		replay_gain_serial = serial;

		if (current_chunk != nullptr) {
			/* flush the current chunk because the new
			   replay gain values affect the following
			   samples */
			FlushChunk();
		}
	} else
		replay_gain_serial = 0;
}

void
DecoderBridge::SubmitMixRamp(MixRampInfo &&mix_ramp)
{
	dc.SetMixRamp(std::move(mix_ramp));
}
