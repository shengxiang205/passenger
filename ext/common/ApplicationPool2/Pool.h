/*
 *  Phusion Passenger - https://www.phusionpassenger.com/
 *  Copyright (c) 2011-2013 Phusion
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
#ifndef _PASSENGER_APPLICATION_POOL2_POOL_H_
#define _PASSENGER_APPLICATION_POOL2_POOL_H_

#include <string>
#include <vector>
#include <utility>
#include <sstream>
#include <iomanip>
#include <boost/thread.hpp>
#include <boost/bind.hpp>
#include <boost/shared_ptr.hpp>
#include <boost/make_shared.hpp>
#include <boost/function.hpp>
#include <boost/foreach.hpp>
#include <boost/date_time/posix_time/posix_time_types.hpp>
#include <oxt/dynamic_thread_group.hpp>
#include <oxt/backtrace.hpp>
#include <ApplicationPool2/Common.h>
#include <ApplicationPool2/Process.h>
#include <ApplicationPool2/Group.h>
#include <ApplicationPool2/SuperGroup.h>
#include <ApplicationPool2/Session.h>
#include <ApplicationPool2/SpawnerFactory.h>
#include <ApplicationPool2/Options.h>
#include <UnionStation.h>
#include <Logging.h>
#include <SafeLibev.h>
#include <Exceptions.h>
#include <RandomGenerator.h>
#include <Utils/Lock.h>
#include <Utils/AnsiColorConstants.h>
#include <Utils/SystemTime.h>
#include <Utils/MessagePassing.h>
#include <Utils/VariantMap.h>
#include <Utils/ProcessMetricsCollector.h>

namespace Passenger {
namespace ApplicationPool2 {

using namespace std;
using namespace boost;
using namespace oxt;


class Pool: public enable_shared_from_this<Pool> {
public:
	struct InspectOptions {
		bool colorize;
		bool verbose;

		InspectOptions()
			: colorize(false),
			  verbose(false)
			{ }

		InspectOptions(const VariantMap &options)
			: colorize(options.getBool("colorize", false, false)),
			  verbose(options.getBool("verbose", false, false))
			{ }
	};

// Actually private, but marked public so that unit tests can access the fields.
public:
	friend class SuperGroup;
	friend class Group;
	typedef UnionStation::LoggerFactory LoggerFactory;
	typedef UnionStation::LoggerFactoryPtr LoggerFactoryPtr;
	typedef UnionStation::LoggerPtr LoggerPtr;

	struct DebugSupport {
		MessageBoxPtr debugger;
		MessageBoxPtr messages;

		// Choose aspects to debug.
		bool restarting;
		bool spawning;
		bool superGroup;

		// The following fields may only be accessed by Pool.
		boost::mutex syncher;
		unsigned int spawnLoopIteration;

		DebugSupport() {
			debugger = make_shared<MessageBox>();
			messages = make_shared<MessageBox>();
			restarting = true;
			spawning   = true;
			superGroup = false;
			spawnLoopIteration = 0;
		}
	};

	typedef shared_ptr<DebugSupport> DebugSupportPtr;

	SpawnerFactoryPtr spawnerFactory;
	LoggerFactoryPtr loggerFactory;
	RandomGeneratorPtr randomGenerator;

	mutable boost::mutex syncher;
	SafeLibev *libev;
	unsigned int max;
	unsigned long long maxIdleTime;
	
	condition_variable garbageCollectionCond;
	
	/**
	 * Code can register background threads in one of these dynamic thread groups
	 * to ensure that threads are interrupted and/or joined properly upon Pool
	 * destruction.
	 * All threads in 'interruptableThreads' will be interrupted and joined upon
	 * Pool destruction.
	 * All threads in 'nonInterruptableThreads' will be joined, but not interrupted,
	 * upon Pool destruction.
	 */
	dynamic_thread_group interruptableThreads;
	dynamic_thread_group nonInterruptableThreads;

	enum LifeStatus {
		ALIVE,
		SHUTTING_DOWN,
		SHUT_DOWN
	} lifeStatus;
	
	SuperGroupMap superGroups;
	
	/**
	 * get() requests that...
	 * - cannot be immediately satisfied because the pool is at full
	 *   capacity and no existing processes can be killed,
	 * - and for which the super group isn't in the pool,
	 * ...are put on this wait list.
	 *
	 * This wait list is processed when one of the following things happen:
	 *
	 * - A process has been spawned but its associated group has
	 *   no get waiters. This process can be killed and the resulting
	 *   free capacity will be used to use spawn a process for this
	 *   get request.
	 * - A process (that has apparently been spawned after getWaitlist
	 *   was populated) is done processing a request. This process can
	 *   then be killed to free capacity.
	 * - A process has failed to spawn, resulting in capacity to
	 *   become free.
	 * - A SuperGroup failed to initialize, resulting in free capacity.
	 * - Someone commanded Pool to detach a process, resulting in free
	 *   capacity.
	 * - Someone commanded Pool to detach a SuperGroup, resulting in
	 *   free capacity.
	 * - The 'max' option has been increased, resulting in free capacity.
	 *
	 * Invariant 1:
	 *    for all options in getWaitlist:
	 *       options.getAppGroupName() is not in 'superGroups'.
	 *
	 * Invariant 2:
	 *    if getWaitlist is non-empty:
	 *       atFullCapacity()
	 * Equivalently:
	 *    if !atFullCapacity():
	 *       getWaitlist is empty.
	 */
	vector<GetWaiter> getWaitlist;

	DebugSupportPtr debugSupport;
	
	static void runAllActions(const vector<Callback> &actions) {
		vector<Callback>::const_iterator it, end = actions.end();
		for (it = actions.begin(); it != end; it++) {
			(*it)();
		}
	}
	
	static void runAllActionsWithCopy(vector<Callback> actions) {
		runAllActions(actions);
	}

	static const char *maybeColorize(const InspectOptions &options, const char *color) {
		if (options.colorize) {
			return color;
		} else {
			return "";
		}
	}
	
	void verifyInvariants() const {
		// !a || b: logical equivalent of a IMPLIES b.
		assert(!( !getWaitlist.empty() ) || ( atFullCapacity(false) ));
		assert(!( !atFullCapacity(false) ) || ( getWaitlist.empty() ));
	}
	
	void verifyExpensiveInvariants() const {
		#ifndef NDEBUG
		vector<GetWaiter>::const_iterator it, end = getWaitlist.end();
		for (it = getWaitlist.begin(); it != end; it++) {
			const GetWaiter &waiter = *it;
			assert(superGroups.get(waiter.options.getAppGroupName()) == NULL);
		}
		#endif
	}

	void fullVerifyInvariants() const {
		TRACE_POINT();
		verifyInvariants();
		UPDATE_TRACE_POINT();
		verifyExpensiveInvariants();
		UPDATE_TRACE_POINT();
		StringMap<SuperGroupPtr>::const_iterator sg_it, sg_end = superGroups.end();
		for (sg_it = superGroups.begin(); sg_it != sg_end; sg_it++) {
			pair<StaticString, SuperGroupPtr> p = *sg_it;
			p.second->verifyInvariants();
			foreach (GroupPtr group, p.second->groups) {
				group->verifyInvariants();
				group->verifyExpensiveInvariants();
			}
		}
	}
	
	ProcessPtr findOldestIdleProcess() const {
		ProcessPtr oldestIdleProcess;
		
		SuperGroupMap::const_iterator it, end = superGroups.end();
		for (it = superGroups.begin(); it != end; it++) {
			const SuperGroupPtr &superGroup = it->second;
			const vector<GroupPtr> &groups = superGroup->groups;
			vector<GroupPtr>::const_iterator g_it, g_end = groups.end();
			for (g_it = groups.begin(); g_it != g_end; g_it++) {
				const GroupPtr &group = *g_it;
				const ProcessList &processes = group->enabledProcesses;
				ProcessList::const_iterator p_it, p_end = processes.end();
				for (p_it = processes.begin(); p_it != p_end; p_it++) {
					const ProcessPtr process = *p_it;
					if (process->utilization() == 0
					     && (oldestIdleProcess == NULL
					         || process->lastUsed < oldestIdleProcess->lastUsed)
					) {
						oldestIdleProcess = process;
					}
				}
			}
		}
		
		return oldestIdleProcess;
	}
	
	ProcessPtr findBestProcessToTrash() const {
		ProcessPtr oldestProcess;
		
		SuperGroupMap::const_iterator it, end = superGroups.end();
		for (it = superGroups.begin(); it != end; it++) {
			const SuperGroupPtr &superGroup = it->second;
			const vector<GroupPtr> &groups = superGroup->groups;
			vector<GroupPtr>::const_iterator g_it, g_end = groups.end();
			for (g_it = groups.begin(); g_it != g_end; g_it++) {
				const GroupPtr &group = *g_it;
				const ProcessList &processes = group->enabledProcesses;
				ProcessList::const_iterator p_it, p_end = processes.end();
				for (p_it = processes.begin(); p_it != p_end; p_it++) {
					const ProcessPtr process = *p_it;
					if (oldestProcess == NULL
					 || process->lastUsed < oldestProcess->lastUsed) {
						oldestProcess = process;
					}
				}
			}
		}
		
		return oldestProcess;
	}
	
	/** Process all waiters on the getWaitlist. Call when capacity has become free.
	 * This function assigns sessions to them by calling get() on the corresponding
	 * SuperGroups, or by creating more SuperGroups, in so far the new capacity allows.
	 */
	void assignSessionsToGetWaiters(vector<Callback> &postLockActions) {
		bool done = false;
		vector<GetWaiter>::iterator it, end = getWaitlist.end();
		vector<GetWaiter> newWaitlist;
		
		for (it = getWaitlist.begin(); it != end && !done; it++) {
			GetWaiter &waiter = *it;
			
			SuperGroup *superGroup = findMatchingSuperGroup(waiter.options);
			if (superGroup != NULL) {
				SessionPtr session = superGroup->get(waiter.options, waiter.callback);
				if (session != NULL) {
					postLockActions.push_back(boost::bind(
						waiter.callback, session, ExceptionPtr()));
				}
				/* else: the callback has now been put in
				 *       the group's get wait list.
				 */
			} else if (!atFullCapacity(false)) {
				createSuperGroupAndAsyncGetFromIt(waiter.options, waiter.callback);
			} else {
				/* Still cannot satisfy this get request. Keep it on the get
				 * wait list and try again later.
				 */
				newWaitlist.push_back(waiter);
			}
		}
		
		getWaitlist = newWaitlist;
	}

	template<typename Queue>
	static void assignExceptionToGetWaiters(Queue &getWaitlist,
		const ExceptionPtr &exception,
		vector<Callback> &postLockActions)
	{
		while (!getWaitlist.empty()) {
			postLockActions.push_back(boost::bind(
				getWaitlist.front().callback, SessionPtr(),
				exception));
			getWaitlist.pop();
		}
	}
	
	void possiblySpawnMoreProcessesForExistingGroups() {
		StringMap<SuperGroupPtr>::const_iterator sg_it, sg_end = superGroups.end();
		/* Looks for Groups that are waiting for capacity to become available,
		 * and spawn processes in those groups.
		 */
		for (sg_it = superGroups.begin(); sg_it != sg_end; sg_it++) {
			pair<StaticString, SuperGroupPtr> p = *sg_it;
			foreach (GroupPtr group, p.second->groups) {
				if (group->isWaitingForCapacity()) {
					P_DEBUG("Group " << group->name << " is waiting for capacity");
					group->spawn();
					if (atFullCapacity(false)) {
						return;
					}
				}
			}
		}
		/* Now look for Groups that haven't maximized their allowed capacity
		 * yet, and spawn processes in those groups.
		 */
		for (sg_it = superGroups.begin(); sg_it != sg_end; sg_it++) {
			pair<StaticString, SuperGroupPtr> p = *sg_it;
			foreach (GroupPtr group, p.second->groups) {
				if (group->shouldSpawn()) {
					P_DEBUG("Group " << group->name << " requests more processes to be spawned");
					group->spawn();
					if (atFullCapacity(false)) {
						return;
					}
				}
			}
		}
	}
	
	void migrateSuperGroupGetWaitlistToPool(const SuperGroupPtr &superGroup) {
		getWaitlist.reserve(getWaitlist.size() + superGroup->getWaitlist.size());
		while (!superGroup->getWaitlist.empty()) {
			getWaitlist.push_back(superGroup->getWaitlist.front());
			superGroup->getWaitlist.pop();
		}
	}
	
	/**
	 * Forcefully destroys and detaches the given SuperGroup. After detaching
	 * the SuperGroup may have a non-empty getWaitlist so be sure to do
	 * something with it.
	 *
	 * Also, one of the post lock actions can potentially perform a long-running
	 * operation, so running them in a thread is advised.
	 */
	void forceDetachSuperGroup(const SuperGroupPtr &superGroup,
		vector<Callback> &postLockActions,
		const SuperGroup::ShutdownCallback &callback)
	{
		const SuperGroupPtr sp = superGroup; // Prevent premature destruction.
		bool removed = superGroups.remove(superGroup->name);
		assert(removed);
		(void) removed; // Shut up compiler warning.
		superGroup->destroy(false, postLockActions, callback);
	}

	bool detachProcessUnlocked(const ProcessPtr &process, vector<Callback> &postLockActions) {
		if (OXT_LIKELY(process->isAlive())) {
			verifyInvariants();
			
			const GroupPtr group = process->getGroup();
			const SuperGroupPtr superGroup = group->getSuperGroup();
			assert(superGroup->state != SuperGroup::INITIALIZING);
			assert(superGroup->getWaitlist.empty());
			
			group->detach(process, postLockActions);
			// 'process' may now be a stale pointer so don't use it anymore.
			assignSessionsToGetWaiters(postLockActions);
			possiblySpawnMoreProcessesForExistingGroups();

			group->verifyInvariants();
			superGroup->verifyInvariants();
			verifyInvariants();
			verifyExpensiveInvariants();
			
			return true;
		} else {
			return false;
		}
	}
	
	void inspectProcessList(const InspectOptions &options, stringstream &result,
		const ProcessList &processes) const
	{
		ProcessList::const_iterator p_it;
		for (p_it = processes.begin(); p_it != processes.end(); p_it++) {
			const ProcessPtr &process = *p_it;
			char buf[128];
			
			snprintf(buf, sizeof(buf),
					"* PID: %-5lu   Sessions: %-2u   Processed: %-5u   Uptime: %s",
					(unsigned long) process->pid,
					process->sessions,
					process->processed,
					process->uptime().c_str());
			result << "  " << buf << endl;

			if (process->enabled == Process::DISABLING) {
				result << "    Disabling..." << endl;
			} else if (process->enabled == Process::DISABLED) {
				result << "    DISABLED" << endl;
			}

			const Socket *socket;
			if (options.verbose && (socket = process->sockets->findSocketWithName("http")) != NULL) {
				result << "    URL     : http://" << replaceString(socket->address, "tcp://", "") << endl;
				result << "    Password: " << process->connectPassword << endl;
			}
		}
	}

	struct DetachSuperGroupWaitTicket {
		boost::mutex syncher;
		condition_variable cond;
		SuperGroup::ShutdownResult result;
		bool done;

		DetachSuperGroupWaitTicket() {
			done = false;
		}
	};

	struct DisableWaitTicket {
		boost::mutex syncher;
		condition_variable cond;
		DisableResult result;
		bool done;

		DisableWaitTicket() {
			done = false;
		}
	};

	static void syncDetachSuperGroupCallback(SuperGroup::ShutdownResult result,
		shared_ptr<DetachSuperGroupWaitTicket> ticket)
	{
		LockGuard l(ticket->syncher);
		ticket->done = true;
		ticket->result = result;
		ticket->cond.notify_one();
	}

	static void waitDetachSuperGroupCallback(shared_ptr<DetachSuperGroupWaitTicket> ticket) {
		ScopedLock l(ticket->syncher);
		while (!ticket->done) {
			ticket->cond.wait(l);
		}
	}

	static void syncDisableProcessCallback(const ProcessPtr &process, DisableResult result,
		shared_ptr<DisableWaitTicket> ticket)
	{
		LockGuard l(ticket->syncher);
		ticket->done = true;
		ticket->result = result;
		ticket->cond.notify_one();
	}

	static void syncGetCallback(Ticket *ticket, const SessionPtr &session, const ExceptionPtr &e) {
		ScopedLock lock(ticket->syncher);
		if (OXT_LIKELY(session != NULL)) {
			ticket->session = session;
		} else {
			ticket->exception = e;
		}
		ticket->cond.notify_one();
	}
	
	SuperGroup *findMatchingSuperGroup(const Options &options) {
		return superGroups.get(options.getAppGroupName()).get();
	}

	static void garbageCollect(PoolPtr self) {
		TRACE_POINT();
		{
			ScopedLock lock(self->syncher);
			self->garbageCollectionCond.timed_wait(lock,
				posix_time::seconds(5));
		}
		while (!this_thread::interruption_requested()) {
			try {
				UPDATE_TRACE_POINT();
				unsigned long long sleepTime = self->realGarbageCollect();
				ScopedLock lock(self->syncher);
				self->garbageCollectionCond.timed_wait(lock,
					posix_time::microseconds(sleepTime));
			} catch (const thread_interrupted &) {
				break;
			} catch (const tracable_exception &e) {
				P_WARN("ERROR: " << e.what() << "\n  Backtrace:\n" << e.backtrace());
			}
		}
	}

	unsigned long long realGarbageCollect() {
		TRACE_POINT();
		ScopedLock lock(syncher);
		SuperGroupMap::iterator it, end = superGroups.end();
		vector<Callback> actions;
		unsigned long long now = SystemTime::getUsec();
		unsigned long long nextGcRunTime = 0;
		
		P_DEBUG("Garbage collection time");
		verifyInvariants();
		
		// For all supergroups and groups...
		for (it = superGroups.begin(); it != end; it++) {
			SuperGroupPtr superGroup = it->second;
			vector<GroupPtr> &groups = superGroup->groups;
			vector<GroupPtr>::iterator g_it, g_end = groups.end();
			
			superGroup->verifyInvariants();
			
			for (g_it = groups.begin(); g_it != g_end; g_it++) {
				GroupPtr group = *g_it;
				ProcessList &processes = group->enabledProcesses;
				ProcessList::iterator p_it, p_end = processes.end();
				
				for (p_it = processes.begin(); p_it != p_end; p_it++) {
					ProcessPtr process = *p_it;
					
					// ...detach processes that have been idle for more than maxIdleTime.
					unsigned long long processGcTime =
						process->lastUsed + maxIdleTime;
					if (process->sessions == 0
					 && now >= processGcTime
					 && (unsigned long) group->enabledCount > group->options.minProcesses) {
						ProcessList::iterator prev = p_it;
						prev--;
						P_DEBUG("Garbage collect idle process: " << process->inspect() <<
							", group=" << group->name);
						group->detach(process, actions);
						p_it = prev;
					} else if (nextGcRunTime == 0
					        || processGcTime < nextGcRunTime) {
						nextGcRunTime = processGcTime;
					}
				}
				
				group->verifyInvariants();
				
				// ...cleanup the spawner if it's been idle for more than preloaderIdleTime.
				if (group->spawner->cleanable()) {
					unsigned long long spawnerGcTime =
						group->spawner->lastUsed() +
						group->options.getMaxPreloaderIdleTime() * 1000000;
					if (now >= spawnerGcTime) {
						P_DEBUG("Garbage collect idle spawner: group=" << group->name);
						group->cleanupSpawner(actions);
					} else if (nextGcRunTime == 0
					        || spawnerGcTime < nextGcRunTime) {
						nextGcRunTime = spawnerGcTime;
					}
				}
			}
			
			superGroup->verifyInvariants();
		}
		
		verifyInvariants();
		lock.unlock();

		// Schedule next garbage collection run.
		unsigned long long sleepTime;
		if (nextGcRunTime == 0 || nextGcRunTime <= now) {
			sleepTime = maxIdleTime;
		} else {
			sleepTime = nextGcRunTime - now;
		}
		P_DEBUG("Garbage collection done; next garbage collect in " <<
			std::fixed << std::setprecision(3) << (sleepTime / 1000000.0) << " sec");

		UPDATE_TRACE_POINT();
		runAllActions(actions);
		UPDATE_TRACE_POINT();
		actions.clear();
		return sleepTime;
	}

	struct ProcessAnalyticsLogEntry {
		string groupName;
		string key;
		stringstream data;
	};

	typedef shared_ptr<ProcessAnalyticsLogEntry> ProcessAnalyticsLogEntryPtr;

	static void collectAnalytics(PoolPtr self) {
		TRACE_POINT();
		syscalls::usleep(3000000);
		while (!this_thread::interruption_requested()) {
			try {
				UPDATE_TRACE_POINT();
				unsigned long long sleepTime = self->realCollectAnalytics();
				syscalls::usleep(sleepTime);
			} catch (const thread_interrupted &) {
				break;
			} catch (const tracable_exception &e) {
				P_WARN("ERROR: " << e.what() << "\n  Backtrace:\n" << e.backtrace());
			}
		}
	}

	static void collectPids(const ProcessList &processes, vector<pid_t> &pids) {
		foreach (const ProcessPtr &process, processes) {
			pids.push_back(process->pid);
		}
	}

	static void updateProcessMetrics(const ProcessList &processes,
		const ProcessMetricMap &allMetrics,
		vector<ProcessPtr> &processesToDetach)
	{
		foreach (const ProcessPtr &process, processes) {
			ProcessMetricMap::const_iterator metrics_it =
				allMetrics.find(process->pid);
			if (metrics_it != allMetrics.end()) {
				process->metrics = metrics_it->second;
			// If the process is missing from 'allMetrics' then either 'ps'
			// failed or the process really is gone. We double check by sending
			// it a signal.
			} else if (!process->dummy && !process->osProcessExists()) {
				P_WARN("Process " << process->inspect() << " no longer exists! "
					"Detaching it from the pool.");
				processesToDetach.push_back(process);
			}
		}
	}

	unsigned long long realCollectAnalytics() {
		TRACE_POINT();
		this_thread::disable_interruption di;
		this_thread::disable_syscall_interruption dsi;
		vector<pid_t> pids;
		unsigned int max;
		
		P_DEBUG("Collecting analytics");
		// Collect all the PIDs.
		{
			UPDATE_TRACE_POINT();
			LockGuard l(syncher);
			max = this->max;
		}
		pids.reserve(max);
		{
			UPDATE_TRACE_POINT();
			LockGuard l(syncher);
			SuperGroupMap::const_iterator sg_it, sg_end = superGroups.end();
			
			for (sg_it = superGroups.begin(); sg_it != sg_end; sg_it++) {
				const SuperGroupPtr &superGroup = sg_it->second;
				vector<GroupPtr>::const_iterator g_it, g_end = superGroup->groups.end();

				for (g_it = superGroup->groups.begin(); g_it != g_end; g_it++) {
					const GroupPtr &group = *g_it;
					collectPids(group->enabledProcesses, pids);
					collectPids(group->disablingProcesses, pids);
					collectPids(group->disabledProcesses, pids);
				}
			}
		}
		
		ProcessMetricMap allMetrics;
		try {
			// Now collect the process metrics and store them in the
			// data structures, and log the state into the analytics logs.
			UPDATE_TRACE_POINT();
			allMetrics = ProcessMetricsCollector().collect(pids);
		} catch (const ProcessMetricsCollector::ParseException &) {
			P_WARN("Unable to collect process metrics: cannot parse 'ps' output.");
			goto end;
		}

		{
			UPDATE_TRACE_POINT();
			vector<ProcessAnalyticsLogEntryPtr> logEntries;
			vector<ProcessPtr> processesToDetach;
			vector<Callback> actions;
			ScopedLock l(syncher);
			SuperGroupMap::iterator sg_it, sg_end = superGroups.end();
			
			UPDATE_TRACE_POINT();
			for (sg_it = superGroups.begin(); sg_it != sg_end; sg_it++) {
				const SuperGroupPtr &superGroup = sg_it->second;
				vector<GroupPtr>::iterator g_it, g_end = superGroup->groups.end();

				for (g_it = superGroup->groups.begin(); g_it != g_end; g_it++) {
					const GroupPtr &group = *g_it;

					updateProcessMetrics(group->enabledProcesses, allMetrics, processesToDetach);
					updateProcessMetrics(group->disablingProcesses, allMetrics, processesToDetach);
					updateProcessMetrics(group->disabledProcesses, allMetrics, processesToDetach);

					// Log to Union Station.
					if (group->options.analytics && loggerFactory != NULL) {
						ProcessAnalyticsLogEntryPtr entry = make_shared<ProcessAnalyticsLogEntry>();
						stringstream &xml = entry->data;

						entry->groupName = group->name;
						entry->key = group->options.unionStationKey;
						xml << "Group: <group>";
						group->inspectXml(xml, false);
						xml << "</group>";
					}
				}
			}

			UPDATE_TRACE_POINT();
			foreach (const ProcessPtr process, processesToDetach) {
				detachProcessUnlocked(process, actions);
			}
			UPDATE_TRACE_POINT();
			processesToDetach.clear();

			l.unlock();
			UPDATE_TRACE_POINT();
			while (!logEntries.empty()) {
				ProcessAnalyticsLogEntryPtr entry = logEntries.back();
				logEntries.pop_back();
				LoggerPtr logger = loggerFactory->newTransaction(entry->groupName,
					"processes", entry->key);
				logger->message(entry->data.str());
			}

			UPDATE_TRACE_POINT();
			runAllActions(actions);
			UPDATE_TRACE_POINT();
			// Run destructors with updated trace point.
			actions.clear();
		}
		
		end:
		// Sleep for about 4 seconds, aligned to seconds boundary
		// for saving power on laptops.
		unsigned long long currentTime = SystemTime::getUsec();
		unsigned long long deadline =
			roundUp<unsigned long long>(currentTime, 1000000) + 4000000;
		P_DEBUG("Analytics collection done; next analytics collection in " <<
			std::fixed << std::setprecision(3) << ((deadline - currentTime) / 1000000.0) <<
			" sec");
		return deadline - currentTime;
	}
	
	SuperGroupPtr createSuperGroup(const Options &options) {
		SuperGroupPtr superGroup = make_shared<SuperGroup>(shared_from_this(),
			options);
		superGroup->initialize();
		superGroups.set(options.getAppGroupName(), superGroup);
		return superGroup;
	}
	
	SuperGroupPtr createSuperGroupAndAsyncGetFromIt(const Options &options,
		const GetCallback &callback)
	{
		SuperGroupPtr superGroup = createSuperGroup(options);
		SessionPtr session = superGroup->get(options, callback);
		/* Callback should now have been put on the wait list,
		 * unless something has changed and we forgot to update
		 * some code here...
		 */
		assert(session == NULL);
		return superGroup;
	}

	// Debugging helper function, implemented in .cpp file so that GDB can access it.
	const SuperGroupPtr getSuperGroup(const char *name);
	
public:
	Pool(SafeLibev *libev, const SpawnerFactoryPtr &spawnerFactory,
		const LoggerFactoryPtr &loggerFactory = LoggerFactoryPtr(),
		const RandomGeneratorPtr &randomGenerator = RandomGeneratorPtr())
	{
		this->libev = libev;
		this->spawnerFactory = spawnerFactory;
		this->loggerFactory = loggerFactory;
		if (randomGenerator != NULL) {
			this->randomGenerator = randomGenerator;
		} else {
			this->randomGenerator = make_shared<RandomGenerator>();
		}
		
		lifeStatus  = ALIVE;
		max         = 6;
		maxIdleTime = 60 * 1000000;
		
		// The following code only serve to instantiate certain inline methods
		// so that they can be invoked from gdb.
		(void) SuperGroupPtr().get();
		(void) GroupPtr().get();
		(void) ProcessPtr().get();
		(void) SessionPtr().get();
	}
	
	~Pool() {
		if (lifeStatus != SHUT_DOWN) {
			P_BUG("You must call Pool::destroy() before actually destroying the Pool object!");
		}
	}

	void initialize() {
		LockGuard l(syncher);
		interruptableThreads.create_thread(
			boost::bind(collectAnalytics, shared_from_this()),
			"Pool analytics collector",
			POOL_HELPER_THREAD_STACK_SIZE
		);
		interruptableThreads.create_thread(
			boost::bind(garbageCollect, shared_from_this()),
			"Pool garbage collector",
			POOL_HELPER_THREAD_STACK_SIZE
		);
	}

	void initDebugging() {
		LockGuard l(syncher);
		debugSupport = make_shared<DebugSupport>();
	}

	void destroy() {
		TRACE_POINT();
		ScopedLock lock(syncher);
		assert(lifeStatus == ALIVE);

		lifeStatus = SHUTTING_DOWN;

		while (!superGroups.empty()) {
			string name = superGroups.begin()->second->name;
			lock.unlock();
			detachSuperGroupByName(name);
			lock.lock();
		}

		UPDATE_TRACE_POINT();
		lock.unlock();
		interruptableThreads.interrupt_and_join_all();
		nonInterruptableThreads.join_all();
		lock.lock();

		lifeStatus = SHUT_DOWN;

		UPDATE_TRACE_POINT();
		verifyInvariants();
		verifyExpensiveInvariants();
	}

	// 'lockNow == false' may only be used during unit tests. Normally we
	// should never call the callback while holding the lock.
	void asyncGet(const Options &options, const GetCallback &callback, bool lockNow = true) {
		DynamicScopedLock lock(syncher, lockNow);
		
		assert(lifeStatus == ALIVE);
		verifyInvariants();
		P_TRACE(2, "asyncGet(appRoot=" << options.appRoot << ")");
		
		SuperGroup *existingSuperGroup = findMatchingSuperGroup(options);
		if (OXT_LIKELY(existingSuperGroup != NULL)) {
			/* Best case: the app super group is already in the pool. Let's use it. */
			P_TRACE(2, "Found existing SuperGroup");
			existingSuperGroup->verifyInvariants();
			SessionPtr session = existingSuperGroup->get(options, callback);
			existingSuperGroup->verifyInvariants();
			verifyInvariants();
			P_TRACE(2, "asyncGet() finished");
			if (lockNow) {
				lock.unlock();
			}
			if (session != NULL) {
				callback(session, ExceptionPtr());
			}
		
		} else if (!atFullCapacity(false)) {
			/* The app super group isn't in the pool and we have enough free
			 * resources to make a new one.
			 */
			P_DEBUG("Spawning new SuperGroup");
			SuperGroupPtr superGroup = createSuperGroupAndAsyncGetFromIt(options, callback);
			superGroup->verifyInvariants();
			verifyInvariants();
			P_DEBUG("asyncGet() finished");
			
		} else {
			vector<Callback> actions;
			
			/* Uh oh, the app super group isn't in the pool but we don't
			 * have the resources to make a new one. The sysadmin should
			 * configure the system to let something like this happen
			 * as least as possible, but let's try to handle it as well
			 * as we can.
			 *
			 * First, try to trash an idle process that's the oldest.
			 */
			P_DEBUG("Pool is at full capacity; trying to free a process...");
			ProcessPtr process = findOldestIdleProcess();
			if (process == NULL) {
				/* All processes are doing something. We have no choice
				 * but to trash a non-idle process.
				 */
				if (options.allowTrashingNonIdleProcesses) {
					process = findBestProcessToTrash();
				}
			} else {
				// Check invariant.
				assert(process->getGroup()->getWaitlist.empty());
			}
			if (process == NULL) {
				/* No process is eligible for killing. This could happen if, for example,
				 * all (super)groups are currently initializing/restarting/spawning/etc.
				 * We have no choice but to satisfy this get() action later when resources
				 * become available.
				 */
				P_DEBUG("Could not free a process; putting request to top-level getWaitlist");
				getWaitlist.push_back(GetWaiter(
					options.copyAndPersist().clearLogger(),
					callback));
			} else {
				GroupPtr group;
				SuperGroupPtr superGroup;
				
				P_DEBUG("Freeing process " << process->inspect());
				group = process->getGroup();
				assert(group != NULL);
				superGroup = group->getSuperGroup();
				assert(superGroup != NULL);
				
				group->detach(process, actions);
				
				/* Now that a process has been trashed we can create
				 * the missing SuperGroup.
				 */
				P_DEBUG("Creating new SuperGroup");
				superGroup = make_shared<SuperGroup>(shared_from_this(), options);
				superGroup->initialize();
				superGroups.set(options.getAppGroupName(), superGroup);
				SessionPtr session = superGroup->get(options, callback);
				/* The SuperGroup is still initializing so the callback
				 * should now have been put on the wait list,
				 * unless something has changed and we forgot to update
				 * some code here...
				 */
				assert(session == NULL);
				group->verifyInvariants();
				superGroup->verifyInvariants();
			}
			
			assert(atFullCapacity(false));
			verifyInvariants();
			verifyExpensiveInvariants();
			P_TRACE(2, "asyncGet() finished");
			
			if (!actions.empty()) {
				if (lockNow) {
					lock.unlock();
					runAllActions(actions);
				} else {
					// This state is not allowed. If we reach
					// here then it probably indicates a bug in
					// the test suite.
					abort();
				}
			}
		}
	}
	
	// TODO: 'ticket' should be a shared_ptr for interruption-safety.
	SessionPtr get(const Options &options, Ticket *ticket) {
		ticket->session.reset();
		ticket->exception.reset();
		
		asyncGet(options, boost::bind(syncGetCallback, ticket, _1, _2));
		
		ScopedLock lock(ticket->syncher);
		while (ticket->session == NULL && ticket->exception == NULL) {
			ticket->cond.wait(lock);
		}
		lock.unlock();
		
		if (OXT_LIKELY(ticket->session != NULL)) {
			SessionPtr session = ticket->session;
			ticket->session.reset();
			return session;
		} else {
			rethrowException(ticket->exception);
			return SessionPtr(); // Shut up compiler warning.
		}
	}
	
	GroupPtr findOrCreateGroup(const Options &options) {
		Options options2 = options;
		options2.noop = true;
		
		Ticket ticket;
		{
			LockGuard l(syncher);
			if (superGroups.get(options.getAppGroupName()) == NULL) {
				// Forcefully create SuperGroup, don't care whether resource limits
				// actually allow it.
				createSuperGroup(options);
			}
		}
		return get(options2, &ticket)->getGroup();
	}
	
	void setMax(unsigned int max) {
		ScopedLock l(syncher);
		assert(max > 0);
		fullVerifyInvariants();
		bool bigger = max > this->max;
		this->max = max;
		if (bigger) {
			/* If there are clients waiting for resources
			 * to become free, spawn more processes now that
			 * we have the capacity.
			 *
			 * We favor waiters on the pool over waiters on the
			 * the groups because the latter already have the
			 * resources to eventually complete. Favoring waiters
			 * on the pool should be fairer.
			 */
			vector<Callback> actions;
			assignSessionsToGetWaiters(actions);
			possiblySpawnMoreProcessesForExistingGroups();
			
			fullVerifyInvariants();
			l.unlock();
			runAllActions(actions);
		} else {
			fullVerifyInvariants();
		}
	}

	void setMaxIdleTime(unsigned long long value) {
		LockGuard l(syncher);
		maxIdleTime = value;
		garbageCollectionCond.notify_all();
	}
	
	unsigned int utilization(bool lock = true) const {
		DynamicScopedLock l(syncher, lock);
		SuperGroupMap::const_iterator it, end = superGroups.end();
		int result = 0;
		for (it = superGroups.begin(); it != end; it++) {
			const SuperGroupPtr &superGroup = it->second;
			result += superGroup->utilization();
		}
		return result;
	}
	
	bool atFullCapacity(bool lock = true) const {
		DynamicScopedLock l(syncher, lock);
		return utilization(false) >= max;
	}

	vector<ProcessPtr> getProcesses(bool lock = true) const {
		DynamicScopedLock l(syncher, lock);
		vector<ProcessPtr> result;
		SuperGroupMap::const_iterator it, end = superGroups.end();
		for (it = superGroups.begin(); OXT_LIKELY(it != end); it++) {
			const SuperGroupPtr &superGroup = it->second;
			vector<GroupPtr> &groups = superGroup->groups;
			vector<GroupPtr>::const_iterator g_it, g_end = groups.end();
			for (g_it = groups.begin(); g_it != g_end; g_it++) {
				const GroupPtr &group = *g_it;
				ProcessList::const_iterator p_it;

				for (p_it = group->enabledProcesses.begin(); p_it != group->enabledProcesses.end(); p_it++) {
					result.push_back(*p_it);
				}
				for (p_it = group->disablingProcesses.begin(); p_it != group->disablingProcesses.end(); p_it++) {
					result.push_back(*p_it);
				}
				for (p_it = group->disabledProcesses.begin(); p_it != group->disabledProcesses.end(); p_it++) {
					result.push_back(*p_it);
				}
			}
		}
		return result;
	}
	
	unsigned int getProcessCount(bool lock = true) const {
		DynamicScopedLock l(syncher, lock);
		unsigned int result = 0;
		SuperGroupMap::const_iterator it, end = superGroups.end();
		for (it = superGroups.begin(); OXT_LIKELY(it != end); it++) {
			const SuperGroupPtr &superGroup = it->second;
			result += superGroup->getProcessCount();
		}
		return result;
	}

	unsigned int getSuperGroupCount() const {
		LockGuard l(syncher);
		return superGroups.size();
	}
	
	SuperGroupPtr findSuperGroupBySecret(const string &secret, bool lock = true) const {
		DynamicScopedLock l(syncher, lock);
		SuperGroupMap::const_iterator it, end = superGroups.end();
		for (it = superGroups.begin(); OXT_LIKELY(it != end); it++) {
			const SuperGroupPtr &superGroup = it->second;
			if (superGroup->secret == secret) {
				return superGroup;
			}
		}
		return SuperGroupPtr();
	}
	
	ProcessPtr findProcessByGupid(const string &gupid, bool lock = true) const {
		vector<ProcessPtr> processes = getProcesses(lock);
		vector<ProcessPtr>::const_iterator it, end = processes.end();
		for (it = processes.begin(); it != end; it++) {
			const ProcessPtr &process = *it;
			if (process->gupid == gupid) {
				return process;
			}
		}
		return ProcessPtr();
	}

	bool detachSuperGroupByName(const string &name) {
		TRACE_POINT();
		ScopedLock l(syncher);
		
		SuperGroupPtr superGroup = superGroups.get(name);
		if (OXT_LIKELY(superGroup != NULL)) {
			if (OXT_LIKELY(superGroups.get(superGroup->name) != NULL)) {
				UPDATE_TRACE_POINT();
				verifyInvariants();
				verifyExpensiveInvariants();
				
				vector<Callback> actions;
				shared_ptr<DetachSuperGroupWaitTicket> ticket =
					make_shared<DetachSuperGroupWaitTicket>();
				ExceptionPtr exception = copyException(
					GetAbortedException("The containg SuperGroup was detached."));

				forceDetachSuperGroup(superGroup, actions,
					boost::bind(syncDetachSuperGroupCallback, _1, ticket));
				assignExceptionToGetWaiters(superGroup->getWaitlist,
					exception, actions);
				#if 0
				/* If this SuperGroup had get waiters, either
				 * on itself or in one of its groups, then we must
				 * reprocess them immediately. Detaching such a
				 * SuperGroup is essentially the same as restarting it.
				 */
				migrateSuperGroupGetWaitlistToPool(superGroup);
				
				UPDATE_TRACE_POINT();
				assignSessionsToGetWaiters(actions);
				#endif
				possiblySpawnMoreProcessesForExistingGroups();
				
				verifyInvariants();
				verifyExpensiveInvariants();
				
				l.unlock();
				UPDATE_TRACE_POINT();
				runAllActions(actions);
				actions.clear();

				UPDATE_TRACE_POINT();
				ScopedLock l2(ticket->syncher);
				while (!ticket->done) {
					ticket->cond.wait(l2);
				}
				return ticket->result == SuperGroup::SUCCESS;
			} else {
				return false;
			}
		} else {
			return false;
		}
	}
	
	bool detachSuperGroupBySecret(const string &superGroupSecret) {
		ScopedLock l(syncher);
		SuperGroupPtr superGroup = findSuperGroupBySecret(superGroupSecret, false);
		if (superGroup != NULL) {
			string name = superGroup->name;
			superGroup.reset();
			l.unlock();
			return detachSuperGroupByName(name);
		} else {
			return false;
		}
	}
	
	bool detachProcess(const ProcessPtr &process) {
		ScopedLock l(syncher);
		vector<Callback> actions;
		bool result = detachProcessUnlocked(process, actions);
		fullVerifyInvariants();
		l.unlock();
		runAllActions(actions);
		return result;
	}

	bool detachProcess(const string &gupid) {
		ScopedLock l(syncher);
		ProcessPtr process = findProcessByGupid(gupid, false);
		if (process != NULL) {
			vector<Callback> actions;
			bool result = detachProcessUnlocked(process, actions);
			fullVerifyInvariants();
			l.unlock();
			runAllActions(actions);
			return result;
		} else {
			return false;
		}
	}

	DisableResult disableProcess(const string &gupid) {
		ScopedLock l(syncher);
		ProcessPtr process = findProcessByGupid(gupid, false);
		if (process != NULL) {
			GroupPtr group = process->getGroup();
			// Must be a shared_ptr to be interruption-safe.
			shared_ptr<DisableWaitTicket> ticket = make_shared<DisableWaitTicket>();
			DisableResult result = group->disable(process,
				boost::bind(syncDisableProcessCallback, _1, _2, ticket));
			group->verifyInvariants();
			group->verifyExpensiveInvariants();
			if (result == DR_DEFERRED) {
				l.unlock();
				ScopedLock l2(ticket->syncher);
				while (!ticket->done) {
					ticket->cond.wait(l2);
				}
				return ticket->result;
			} else {
				return result;
			}
		} else {
			return DR_NOOP;
		}
	}

	unsigned int restartGroupsByAppRoot(const string &appRoot) {
		ScopedLock l(syncher);
		SuperGroupMap::iterator sg_it, sg_end = superGroups.end();
		unsigned int result = 0;

		for (sg_it = superGroups.begin(); sg_it != sg_end; sg_it++) {
			const SuperGroupPtr &superGroup = sg_it->second;
			foreach (const GroupPtr &group, superGroup->groups) {
				if (group->options.appRoot == appRoot) {
					result++;
					if (!group->restarting()) {
						group->restart(group->options);
					}
				}
			}
		}

		return result;
	}

	unsigned int restartSuperGroupsByAppRoot(const string &appRoot) {
		ScopedLock l(syncher);
		SuperGroupMap::iterator sg_it, sg_end = superGroups.end();
		unsigned int result = 0;

		for (sg_it = superGroups.begin(); sg_it != sg_end; sg_it++) {
			const SuperGroupPtr &superGroup = sg_it->second;
			if (superGroup->options.appRoot == appRoot) {
				result++;
				superGroup->restart(superGroup->options);
			}
		}

		return result;
	}

	/**
	 * Checks whether at least one process is being spawned.
	 */
	bool isSpawning(bool lock = true) const {
		DynamicScopedLock l(syncher, lock);
		SuperGroupMap::const_iterator it, end = superGroups.end();
		for (it = superGroups.begin(); it != end; it++) {
			foreach (GroupPtr group, it->second->groups) {
				if (group->spawning()) {
					return true;
				}
			}
		}
		return false;
	}

	string inspect(const InspectOptions &options = InspectOptions(), bool lock = true) const {
		DynamicScopedLock l(syncher, lock);
		stringstream result;
		const char *headerColor = maybeColorize(options, ANSI_COLOR_YELLOW ANSI_COLOR_BLUE_BG ANSI_COLOR_BOLD);
		const char *resetColor  = maybeColorize(options, ANSI_COLOR_RESET);
		
		result << headerColor << "----------- General information -----------" << resetColor << endl;
		result << "Max pool size : " << max << endl;
		result << "Processes     : " << getProcessCount(false) << endl;
		result << "Requests in top-level queue : " << getWaitlist.size() << endl;
		if (options.verbose) {
			unsigned int i = 0;
			foreach (const GetWaiter &waiter, getWaitlist) {
				result << "  " << i << ": " << waiter.options.getAppGroupName() << endl;
				i++;
			}
		}
		result << endl;
		
		result << headerColor << "----------- Application groups -----------" << resetColor << endl;
		SuperGroupMap::const_iterator sg_it, sg_end = superGroups.end();
		for (sg_it = superGroups.begin(); sg_it != sg_end; sg_it++) {
			const SuperGroupPtr &superGroup = sg_it->second;
			const Group *group = superGroup->defaultGroup;
			ProcessList::const_iterator p_it;
			
			if (group != NULL) {
				result << group->name << ":" << endl;
				result << "  App root: " << group->options.appRoot << endl;
				if (group->restarting()) {
					result << "  (restarting...)" << endl;
				}
				if (group->spawning()) {
					result << "  (spawning new process...)" << endl;
				}
				result << "  Requests in queue: " << group->getWaitlist.size() << endl;
				inspectProcessList(options, result, group->enabledProcesses);
				inspectProcessList(options, result, group->disablingProcesses);
				inspectProcessList(options, result, group->disabledProcesses);
				result << endl;
			}
		}
		return result.str();
	}

	string toXml(bool includeSecrets = true, bool lock = true) const {
		DynamicScopedLock l(syncher, lock);
		stringstream result;
		SuperGroupMap::const_iterator sg_it;
		vector<GroupPtr>::const_iterator g_it;
		ProcessList::const_iterator p_it;

		result << "<?xml version=\"1.0\" encoding=\"iso8859-1\" ?>\n";
		result << "<info version=\"2\">";
		
		result << "<process_count>" << getProcessCount(false) << "</process_count>";
		result << "<max>" << max << "</max>";
		result << "<utilization>" << utilization(false) << "</utilization>";
		result << "<get_wait_list_size>" << getWaitlist.size() << "</get_wait_list_size>";

		if (includeSecrets) {
			vector<GetWaiter>::const_iterator w_it, w_end = getWaitlist.end();

			result << "<get_wait_list>";
			for (w_it = getWaitlist.begin(); w_it != w_end; w_it++) {
				const GetWaiter &waiter = *w_it;
				result << "<item>";
				result << "<app_group_name>" << escapeForXml(waiter.options.getAppGroupName()) << "</app_group_name>";
				result << "</item>";
			}
			result << "</get_wait_list>";
		}
		
		result << "<supergroups>";
		for (sg_it = superGroups.begin(); sg_it != superGroups.end(); sg_it++) {
			const SuperGroupPtr &superGroup = sg_it->second;

			result << "<supergroup>";
			result << "<name>" << escapeForXml(superGroup->name) << "</name>";
			result << "<state>" << superGroup->getStateName() << "</state>";
			result << "<get_wait_list_size>" << superGroup->getWaitlist.size() << "</get_wait_list_size>";
			result << "<utilization>" << superGroup->utilization() << "</utilization>";
			if (includeSecrets) {
				result << "<secret>" << escapeForXml(superGroup->secret) << "</secret>";
			}

			for (g_it = superGroup->groups.begin(); g_it != superGroup->groups.end(); g_it++) {
				const GroupPtr &group = *g_it;

				if (group->componentInfo.isDefault) {
					result << "<group default=\"true\">";
				} else {
					result << "<group>";
				}
				group->inspectXml(result, includeSecrets);
				result << "</group>";
			}
			result << "</supergroup>";
		}
		result << "</supergroups>";

		result << "</info>";
		return result.str();
	}
};


} // namespace ApplicationPool2
} // namespace Passenger

#endif /* _PASSENGER_APPLICATION_POOL2_POOL_H_ */
