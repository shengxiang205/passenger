/*
 *  Phusion Passenger - https://www.phusionpassenger.com/
 *  Copyright (c) 2011, 2012 Phusion
 *
 *  "Phusion Passenger" is a trademark of Hongli Lai & Ninh Bui.
 *
 *  Permission is hereby granted, free of charge, to any person obtaining a copy
 *  of this software and associated documentation files (the "Software"), to deal
 *  in the Software without restriction, including without limitation the rights
 *  to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 *  copies of the Software, and to permit persons to whom the Software is
 *  furnished to do so, subject to the following conditions:
 *
 *  The above copyright notice and this permission notice shall be included in
 *  all copies or substantial portions of the Software.
 *
 *  THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 *  IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 *  FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 *  AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 *  LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 *  OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
 *  THE SOFTWARE.
 */
#ifndef _EVENTED_BUFFERED_INPUT_H_
#define _EVENTED_BUFFERED_INPUT_H_

#include <cstdio>
#include <cstddef>
#include <cassert>

#include <sstream>
#include <string>

#include <boost/bind.hpp>
#include <boost/enable_shared_from_this.hpp>
#include <boost/shared_ptr.hpp>
#include <boost/enable_shared_from_this.hpp>
#include <oxt/system_calls.hpp>
#include <ev++.h>

#include <SafeLibev.h>
#include <FileDescriptor.h>
#include <StaticString.h>
#include <Logging.h>
#include <Utils/StrIntUtils.h>

namespace Passenger {

using namespace boost;
using namespace oxt;

#define EBI_TRACE(expr) P_TRACE(3, "[EventedBufferedInput " << this << " " << inspect() << "] " << expr)

/**
 * Provides input buffering services for non-blocking sockets in evented I/O systems.
 *
 * Wrap an EventedBufferedInput around a socket and provide a data handler callback.
 * The handler is called every time there is incoming socket data. The handler must return
 * the number of bytes that it has actually consumed. If not everything has been
 * consumed, then the handler will be called with the remaining data in the next
 * tick.
 *
 * TODO: this code is directly ported from Zangetsu's socket_input_wrapper.js. We
 * should port over the unit tests too.
 */
template<size_t bufferSize = 1024 * 8>
class EventedBufferedInput: public enable_shared_from_this< EventedBufferedInput<bufferSize> > {
private:
	enum State {
		LIVE,
		END_OF_STREAM,
		READ_ERROR,
		CLOSED
	};

	SafeLibev *libev;
	FileDescriptor fd;
	ev::io watcher;
	StaticString buffer;
	State state;
	bool paused;
	bool socketPaused;
	bool nextTickInstalled;
	int error;
	char bufferData[bufferSize];

	void resetCallbackFields() {
		onData = NULL;
		onError = NULL;
		userData = NULL;
	}

	void onReadable(ev::io &watcher, int revents) {
		// Keep 'this' alive until function exit.
		shared_ptr< EventedBufferedInput<bufferSize> > self = EventedBufferedInput<bufferSize>::shared_from_this();

		EBI_TRACE("onReadable");
		ssize_t ret = syscalls::read(fd, bufferData, bufferSize);
		if (ret == -1) {
			if (errno != EAGAIN) {
				error = errno;
				EBI_TRACE("read error " << error << " (" << strerror(error) << ")");
				assert(state == LIVE);
				assert(!socketPaused);
				assert(buffer.empty());
				assert(!paused);

				watcher.stop();
				state = READ_ERROR;
				paused = true;
				if (onError != NULL) {
					onError(self, "Cannot read from socket", error);
				}
			}

		} else if (ret == 0) {
			EBI_TRACE("end of stream");
			assert(state == LIVE);
			assert(!socketPaused);
			assert(buffer.empty());
			assert(!paused);

			watcher.stop();
			state = END_OF_STREAM;
			paused = true;
			onData(self, StaticString());

		} else {
			EBI_TRACE("read " << ret << " bytes");
			assert(state == LIVE);
			assert(!socketPaused);
			assert(buffer.empty());
			assert(!paused);

			buffer = StaticString(bufferData, ret);
			processBuffer();
		}
	}

	void processBufferInNextTick() {
		if (!nextTickInstalled) {
			nextTickInstalled = true;
			libev->runAsync(boost::bind(
				realProcessBufferInNextTick,
				weak_ptr< EventedBufferedInput<bufferSize> >(this->shared_from_this())
			));
		}
	}

	static void realProcessBufferInNextTick(weak_ptr< EventedBufferedInput<bufferSize> > wself) {
		shared_ptr< EventedBufferedInput<bufferSize> > self = wself.lock();
		if (self != NULL) {
			self->nextTickInstalled = false;
			self->processBuffer();
		}
	}

	void processBuffer() {
		EBI_TRACE("processBuffer");
		if (state == CLOSED) {
			return;
		}
		assert(state == LIVE);
		if (paused || buffer.empty() || fd == -1) {
			return;
		}

		assert(buffer.size() > 0);
		size_t consumed = onData(EventedBufferedInput<bufferSize>::shared_from_this(),
			buffer);
		EBI_TRACE("Consumed " << consumed << " bytes");
		if (state == CLOSED) {
			return;
		}
		if (consumed == buffer.size()) {
			buffer = StaticString();
			if (!paused && socketPaused) {
				socketPaused = false;
				watcher.start();
			}
		} else {
			buffer = buffer.substr(consumed);
			if (!socketPaused) {
				socketPaused = true;
				watcher.stop();
			}
			if (!paused) {
				// Consume rest of the data in the next tick.
				EBI_TRACE("Consume rest in next tick");
				processBufferInNextTick();
			}
		}
	}

	void _reset(SafeLibev *libev, const FileDescriptor &fd) {
		this->libev = libev;
		this->fd = fd;
		buffer = StaticString();
		state = LIVE;
		paused = true;
		socketPaused = true;
		nextTickInstalled = false;
		error = 0;
		if (watcher.is_active()) {
			watcher.stop();
		}
		if (libev != NULL) {
			watcher.set(libev->getLoop());
		}
		if (fd != -1) {
			watcher.set(fd, ev::READ);
		}
	}

public:
	typedef size_t (*DataCallback)(const shared_ptr< EventedBufferedInput<bufferSize> > &source, const StaticString &data);
	typedef void (*ErrorCallback)(const shared_ptr< EventedBufferedInput<bufferSize> > &source, const char *message, int errnoCode);

	DataCallback onData;
	ErrorCallback onError;
	void *userData;

	EventedBufferedInput() {
		resetCallbackFields();
		_reset(NULL, FileDescriptor());
		watcher.set<EventedBufferedInput<bufferSize>,
			&EventedBufferedInput<bufferSize>::onReadable>(this);
		EBI_TRACE("created");
	}

	EventedBufferedInput(SafeLibev *libev, const FileDescriptor &fd) {
		resetCallbackFields();
		_reset(libev, fd);
		watcher.set<EventedBufferedInput<bufferSize>,
			&EventedBufferedInput<bufferSize>::onReadable>(this);
		EBI_TRACE("created");
	}

	~EventedBufferedInput() {
		watcher.stop();
		EBI_TRACE("destroyed");
	}

	bool resetable() const {
		return !nextTickInstalled;
	}

	void reset(SafeLibev *libev, const FileDescriptor &fd) {
		EBI_TRACE("reset()");
		_reset(libev, fd);
	}

	void stop() {
		if (state == LIVE && !paused) {
			EBI_TRACE("stop()");
			paused = true;
			if (!socketPaused) {
				socketPaused = true;
				watcher.stop();
			}
		}
	}

	void start() {
		if (state == LIVE && paused) {
			EBI_TRACE("start()");
			assert(socketPaused);
			
			paused = false;
			if (!buffer.empty()) {
				processBufferInNextTick();
			} else {
				socketPaused = false;
				watcher.start();
			}
		}
	}

	bool isStarted() const {
		return !paused;
	}

	bool endReached() const {
		return state == END_OF_STREAM;
	}

	const FileDescriptor &getFd() const {
		return fd;
	}

	string inspect() const {
		stringstream result;
		const char *stateStr;

		result << "fd=" << fd;

		switch (state) {
		case LIVE:
			stateStr = "LIVE";
			break;
		case END_OF_STREAM:
			stateStr = "END_OF_STREAM";
			break;
		case READ_ERROR:
			stateStr = "READ_ERROR";
			break;
		case CLOSED:
			stateStr = "CLOSED";
			break;
		default:
			stateStr = "UNKNOWN";
			break;
		}
		result << ", state=" << stateStr;

		result << ", buffer(" << buffer.size() << ")=\"" << cEscapeString(buffer) << "\"";
		result << ", paused=" << paused;
		result << ", socketPaused=" << socketPaused;
		result << ", nextTickInstalled=" << nextTickInstalled;
		result << ", error=" << error;

		return result.str();
	}
};

typedef shared_ptr< EventedBufferedInput<> > EventedBufferedInputPtr;

} // namespace Passenger

#endif /* _EVENTED_BUFFERED_INPUT_H_ */
