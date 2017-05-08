#ifndef _NANOX_LOCALITY_LOCALITY_COMMON
#define _NANOX_LOCALITY_LOCALITY_COMMON

#include <cstdlib>
#include <pthread.h>

namespace nanos {
  namespace ext {
    namespace locsched {

      struct SocketInfo
      {
        int weight;
        std::vector<int> distances;
      };
      
      class DepSocket
      {
        int _socket;
        
      public:
        DepSocket(int socket=0) : _socket(socket) {}
        DepSocket(const DepSocket & o) : _socket(o._socket) {}

        DepSocket & force(int newSocket) {
          _socket = newSocket;
          return *this;
        }
        
        DepSocket & operator=(int newSocket) {
          if (_socket == 0 or (_socket < 0 and newSocket > 0))
            _socket = newSocket;
          
          return *this;
        }
        
        operator int() const { return std::abs(_socket); }
        
        bool isFinal() const { return _socket > 0; }
      };

      class RWLock
      {
      private:
        pthread_rwlock_t _ptlock;
        RWLock(const RWLock &) {}

        void release() { pthread_rwlock_unlock(&_ptlock); }
      public:
        RWLock() {
          pthread_rwlockattr_t attr;
          pthread_rwlockattr_init(&attr);
          pthread_rwlockattr_setkind_np(&attr, PTHREAD_RWLOCK_PREFER_WRITER_NONRECURSIVE_NP);
          pthread_rwlock_init(&_ptlock, &attr);
          pthread_rwlockattr_destroy(&attr);
        }

        ~RWLock() { pthread_rwlock_destroy(&_ptlock); }
        void acquireRead() { pthread_rwlock_rdlock(&_ptlock); }
        void acquireWrite() { pthread_rwlock_wrlock(&_ptlock); }
        void releaseWrite() { release(); }
        void releaseRead() { release(); }
      };
    }
  }
}

#endif
