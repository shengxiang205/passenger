/*
 *  Phusion Passenger - https://www.phusionpassenger.com/
 *  Copyright (c) 2013 Phusion
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
#ifndef _PASSENGER_APPLICATION_POOL2_APP_TYPES_H_
#define _PASSENGER_APPLICATION_POOL2_APP_TYPES_H_

#ifdef __cplusplus
extern "C" {
#endif /* __cplusplus */

typedef enum {
	PAT_RACK,
	PAT_WSGI,
	PAT_CLASSIC_RAILS,
	PAT_NONE
} PassengerAppType;

typedef void PassengerAppTypeDetector;

PassengerAppTypeDetector *passenger_app_type_detector_new();
void passenger_app_type_detector_free(PassengerAppTypeDetector *detector);
PassengerAppType passenger_app_type_detector_check_document_root(PassengerAppTypeDetector *detector,
	const char *documentRoot, unsigned int len, int resolveFirstSymlink);
PassengerAppType passenger_app_type_detector_check_app_root(PassengerAppTypeDetector *detector,
	const char *appRoot, unsigned int len);

const char *passenger_get_app_type_name(PassengerAppType type);

#ifdef __cplusplus
}
#endif /* __cplusplus */


#ifdef __cplusplus
#include <oxt/macros.hpp>
#include <oxt/backtrace.hpp>
#include <cstdlib>
#include <Logging.h>
#include <StaticString.h>
#include <Utils.h>
#include <Utils/StrIntUtils.h>
#include <Utils/CachedFileStat.hpp>

namespace Passenger {
namespace ApplicationPool2 {


struct AppTypeDefinition {
	const PassengerAppType type;
	const char * const name;
	const char * const startupFile;
	const char * const processTitle;
};

extern const AppTypeDefinition appTypeDefinitions[];


class AppTypeDetector {
private:
	CachedFileStat *cstat;
	unsigned int throttleRate;
	bool ownsCstat;

	bool check(char *buf, const char *end, const StaticString &appRoot, const char *name) {
		char *pos = buf;
		pos = appendData(pos, end, appRoot);
		pos = appendData(pos, end, "/");
		pos = appendData(pos, end, name);
		if (OXT_UNLIKELY(pos == end)) {
			TRACE_POINT();
			P_CRITICAL("BUG: buffer overflow");
			abort();
		}
		return fileExists(StaticString(buf, pos - buf), cstat, throttleRate);
	}

public:
	AppTypeDetector() {
		cstat = new CachedFileStat();
		ownsCstat = true;
		throttleRate = 1;
	}

	AppTypeDetector(CachedFileStat *_cstat, unsigned int _throttleRate) {
		cstat = _cstat;
		ownsCstat = false;
		throttleRate = _throttleRate;
	}

	~AppTypeDetector() {
		if (ownsCstat) {
			delete cstat;
		}
	}

	PassengerAppType checkDocumentRoot(const StaticString &documentRoot, bool resolveFirstSymlink = false) {
		if (!resolveFirstSymlink) {
			return checkAppRoot(extractDirNameStatic(documentRoot));
		} else {
			char ntDocRoot[documentRoot.size() + 1];
			memcpy(ntDocRoot, documentRoot.data(), documentRoot.size());
			ntDocRoot[documentRoot.size()] = '\0';
			string resolvedDocumentRoot = resolveSymlink(ntDocRoot);
			return checkAppRoot(extractDirNameStatic(resolvedDocumentRoot));
		}
	}

	PassengerAppType checkAppRoot(const StaticString &appRoot) {
		char buf[appRoot.size() + 32];
		const char *end = buf + appRoot.size() + 32;
		const AppTypeDefinition *definition = &appTypeDefinitions[0];

		while (definition->type != PAT_NONE) {
			if (check(buf, end, appRoot, definition->startupFile)) {
				return definition->type;
			}
			definition++;
		}
		return PAT_NONE;
	}
};


inline const char *
getAppTypeName(PassengerAppType type) {
	const AppTypeDefinition *definition = &appTypeDefinitions[0];

	while (definition->type != PAT_NONE) {
		if (definition->type == type) {
			return definition->name;
		}
		definition++;
	}
	return NULL;
}

inline PassengerAppType
getAppType(const StaticString &name) {
	const AppTypeDefinition *definition = &appTypeDefinitions[0];

	while (definition->type != PAT_NONE) {
		if (name == definition->name) {
			return definition->type;
		}
		definition++;
	}
	return PAT_NONE;
}

inline const char *
getAppTypeStartupFile(PassengerAppType type) {
	const AppTypeDefinition *definition = &appTypeDefinitions[0];

	while (definition->type != PAT_NONE) {
		if (definition->type == type) {
			return definition->startupFile;
		}
		definition++;
	}
	return NULL;
}

inline const char *
getAppTypeProcessTitle(PassengerAppType type) {
	const AppTypeDefinition *definition = &appTypeDefinitions[0];

	while (definition->type != PAT_NONE) {
		if (definition->type == type) {
			return definition->processTitle;
		}
		definition++;
	}
	return NULL;
}


} // namespace ApplicationPool2
} // namespace Passenger
#endif /* __cplusplus */

#endif /* _PASSENGER_APPLICATION_POOL2_APP_TYPES_H_ */
