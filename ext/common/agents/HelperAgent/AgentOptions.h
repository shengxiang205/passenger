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
#ifndef _PASSENGER_HELPER_AGENT_OPTIONS_H_
#define _PASSENGER_HELPER_AGENT_OPTIONS_H_

#include <sys/types.h>
#include <string>
#include <Utils/VariantMap.h>
#include <Utils/Base64.h>

namespace Passenger {

using namespace std;


struct AgentOptions {
	pid_t   webServerPid;
	string  tempDir;
	bool    userSwitching;
	string  defaultUser;
	string  defaultGroup;
	string  passengerRoot;
	string  rubyCommand;
	unsigned int generationNumber;
	unsigned int maxPoolSize;
	unsigned int maxInstancesPerApp;
	unsigned int poolIdleTime;
	string requestSocketPassword;
	string messageSocketPassword;
	string loggingAgentAddress;
	string loggingAgentPassword;
	string prestartUrls;

	AgentOptions() { }

	AgentOptions(const VariantMap &options) {
		webServerPid  = options.getPid("web_server_pid");
		tempDir       = options.get("temp_dir");
		userSwitching = options.getBool("user_switching");
		defaultUser   = options.get("default_user");
		defaultGroup  = options.get("default_group");
		passengerRoot = options.get("passenger_root");
		rubyCommand   = options.get("ruby");
		generationNumber   = options.getInt("generation_number");
		maxPoolSize        = options.getInt("max_pool_size");
		maxInstancesPerApp = options.getInt("max_instances_per_app");
		poolIdleTime       = options.getInt("pool_idle_time");
		requestSocketPassword = Base64::decode(options.get("request_socket_password"));
		messageSocketPassword = Base64::decode(options.get("message_socket_password"));
		loggingAgentAddress   = options.get("logging_agent_address");
		loggingAgentPassword  = options.get("logging_agent_password");
		prestartUrls          = options.get("prestart_urls");
	}
};


} // namespace Passenger

#endif /* _PASSENGER_HELPER_AGENT_OPTIONS_H_ */
