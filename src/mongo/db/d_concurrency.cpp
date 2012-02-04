// @file d_concurrency.cpp 

#include "pch.h"
#include "../util/concurrency/qlock.h"
#include "d_concurrency.h"
#include "../util/concurrency/threadlocal.h"
#include "../util/concurrency/rwlock.h"
#include "../util/concurrency/mapsf.h"
#include "../util/assert_util.h"
#include "client.h"
#include "namespacestring.h"
#include "d_globals.h"
#include "mongomutex.h"

// oplog locking
// no top level read locks
// system.profile writing
// oplog now
// yielding
// commitIfNeeded

namespace mongo { 

    static QLock& q = *new QLock();

    Lock::Nongreedy::Nongreedy() {
        q.stop_greed();
    }
    Lock::Nongreedy::~Nongreedy() {
        q.start_greed();
    }

    //static RWLockRecursive excludeWrites("excludeWrites");
    //static mapsf<string,RWLockRecursive*> dblocks;

    // todo : report HLMutex status in db.currentOp() output
    HLMutex::HLMutex(const char *name) : SimpleMutex(name)
    { }

    // 0, 'r', 'w', 'R', 'W'
    __declspec( thread ) char threadState;
    
    static bool lock_W_try(int ms) { 
        assert( threadState == 0 );
        bool got = q.lock_W(ms);
        if( got ) 
            threadState = 'W';
        return got;
    }
    static void lock_W() { 
        assert( threadState == 0 );
        threadState = 'W';
        q.lock_W();
    }
    static void unlock_W() { 
        wassert( threadState == 'W' );
        threadState = 0;
        q.unlock_W();
    }
    static void lock_R() { 
        assert( threadState == 0 );
        threadState = 'R';
        q.lock_R();
    }
    static void unlock_R() { 
        assert( threadState == 'R' );
        threadState = 0;
        q.unlock_R();
    }
    static void lock_w() { 
        assert( threadState == 0 );
        threadState = 'w';
        q.lock_w();
    }
    static void unlock_w() { 
        assert( threadState == 'w' );
        threadState = 0;
        q.unlock_w();
    }

    // these are safe for use ACROSS threads.  i.e. one thread can lock and 
    // another unlock!
    void Lock::ThreadSpan::setWLockedNongreedy() { 
        assert( threadState == 0 ); // as this spans threads the tls wouldn't make sense
        q.lock_W();
        q.stop_greed();
    }
    void Lock::ThreadSpan::W_to_R() { 
        assert( threadState == 0 );
        q.lock_W();
        q.stop_greed();
    }
    void Lock::ThreadSpan::unsetW() {
        assert( threadState == 0 );
        q.unlock_W();
        q.start_greed();
    }
    void Lock::ThreadSpan::unsetR() {
        assert( threadState == 0 );
        q.unlock_R();
        q.start_greed();
    }

    int Lock::isLocked() {
        return threadState;
    }
    int Lock::isWriteLocked() { // w or W
        return threadState & 'W'; // ascii assumed
    }

    Lock::GlobalWrite::TempRelease::TempRelease() {
        unlock_W();
    }
    Lock::GlobalWrite::TempRelease::~TempRelease() {
        DESTRUCTOR_GUARD( lock_W(); )
    }
    Lock::GlobalWrite::GlobalWrite() : already(threadState == 'W') {
        if( !already ) 
            lock_W();
    }
    Lock::GlobalWrite::~GlobalWrite() {
        if( !already ) 
            unlock_W();
    }
    Lock::GlobalRead::GlobalRead() : already(threadState == 'R' || threadState == 'W') {
        if( !already )
            lock_R();
    }
    Lock::GlobalRead::~GlobalRead() {
        if( !already ) 
            unlock_R();
    }
    Lock::DBWrite::DBWrite(const StringData& ns) { 
        // TEMP : 
        lock_W();
        // todo: 
        //lock_w();
        //lock_the_db
    }
    Lock::DBWrite::~DBWrite() { 
        // TEMP : 
        unlock_W();
    }
    Lock::DBRead::DBRead(const StringData& ns) { 
        // TEMP : 
        lock_R();
    } 
    Lock::DBRead::~DBRead() { 
        unlock_R();
    }

}

namespace mongo { 

    writelock::writelock() { 
        lk1.reset( new Lock::GlobalWrite() );
    }
    writelock::writelock(const string& ns) { 
        if( ns.empty() ) { 
            lk1.reset( new Lock::GlobalWrite() );
        }
        else {
            lk2.reset( new Lock::DBWrite(ns) );
        }
    }

    writelocktry::writelocktry( int tryms ) : 
        _got( lock_W_try(tryms) )
    { }
    writelocktry::~writelocktry() { 
        if( _got )
            unlock_W();
    }

    readlock::readlock() {
        lk1.reset( new Lock::GlobalRead() );
    }
    readlock::readlock(const string& ns) {
        if( ns.empty() ) { 
            lk1.reset( new Lock::GlobalRead() );
        }
        else {
            lk2.reset( new Lock::DBRead(ns) );
        }
    }

}

namespace mongo {

    /* backward compatible glue. it could be that the assumption was that 
       it's a global read lock, so 'r' and 'w' don't qualify.
       */ 
    bool MongoMutex::atLeastReadLocked() { 
        int x = Lock::isLocked();
        return x == 'R' || x == 'W';
    }
    bool MongoMutex::isWriteLocked() { 
        return Lock::isLocked() == 'W';
    }

    // this tie-in temporary until MongoMutex is folded in more directly.Exclud
    // called when the lock has been achieved
    void MongoMutex::lockedExclusively() {
        Client& c = cc();
        curopGotLock(&c); // hopefully lockStatus replaces one day
        _minfo.entered(); // hopefully eliminate one day 
    }

    void MongoMutex::unlockingExclusively() {
        Client& c = cc();
        _minfo.leaving();
    }

    MongoMutex::MongoMutex(const char *name) : _m(name) {
        static int n = 0;
        assert( ++n == 1 ); // below releasingWriteLock we assume MongoMutex is a singleton, and uses dbMutex ref above
        _remapPrivateViewRequested = false;
    }

}
