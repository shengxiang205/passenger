#ifndef _PASSENGER_LOCK_H_
#define _PASSENGER_LOCK_H_

#include <boost/thread.hpp>

namespace Passenger {

using namespace boost;

/** Shortcut typedefs. */
typedef lock_guard<boost::mutex> LockGuard;
typedef unique_lock<boost::mutex> ScopedLock;

/** Nicer syntax for conditionally locking the mutex during construction. */
class DynamicScopedLock: public unique_lock<boost::mutex> {
public:
	DynamicScopedLock(boost::mutex &m, bool lockNow = true)
		: unique_lock<boost::mutex>(m, defer_lock)
	{
		if (lockNow) {
			lock();
		}
	}
};

} // namespace Passenger

#endif /* _PASSENGER_LOCK_H_ */
