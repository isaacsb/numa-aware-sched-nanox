#ifndef _NANOX_LOCALITY_DEP_METHOD
#define _NANOX_LOCALITY_DEP_METHOD

#include "workdescriptor.hpp"
#include "dependableobject.hpp"
#include "hashmap.hpp"
#include "config.hpp"

#include "depsregion.hpp"

#include "locality-common.hpp"

#include <tr1/random>
#include <cassert>

#ifdef LOCALITY_USE_NUMA
#include "numaif.h"
#include <cstring>
#include <cerrno>
#include <unistd.h>
#endif

#define RAISE_EVENT(event, x)                                           \
  NANOS_INSTRUMENT(sys.getInstrumentation()->raiseOpenBurstEvent((event), (x));)
#define CLOSE_EVENT(event)                                              \
  NANOS_INSTRUMENT(sys.getInstrumentation()->raiseCloseBurstEvent((event), 0);)

namespace nanos {
  namespace ext {
    namespace locsched {
      class DepMethod {
      public:
        enum NextMode {
          RANDOM,
          SOCKET_CYCLIC,
          CPU_CYCLIC,
        };

        struct DepInfo {
          int queueNum;
          DepInfo(int q=0)
            : queueNum(q)
          {}

          void reset() { queueNum = 0; }
        };
      private:
        typedef DependableObject::TargetVector::const_iterator _tvIt;
        typedef DependableObject::DependableObjectVector::const_iterator _dovIt;

        bool _init;
        int _numSockets;
        int _numThreads;
        size_t _minDepSize;
        bool _deleteRand;
        mutable std::tr1::mt19937 * _rand;
        mutable Atomic<int> _nextSocket;
        const NextMode _nextMode;
        DepInfo & (*_depInfo) (WD &);
        HashMap< const void *, DepSocket > _dependencySocket;

        Lock _initLock;
        

#ifdef NANOS_INSTRUMENTATION_ENABLED
        static Lock _generalLock;
        static bool _generalInit;
        static nanos_event_key_t _movePagesEvent;
#endif
        
      public:
        DepMethod(NextMode m=RANDOM)
          : _init(false),
            _numSockets(0),
            _numThreads(0),
            _minDepSize(0),
            _deleteRand(false),
            _rand(NULL),
            _nextSocket(0),
            _nextMode(m),
            _depInfo(NULL)
        {}
        void numSockets(int n) { _numSockets = n; }
        int numSockets() const { return _numSockets; }
        void numThreads(int n) { _numThreads = n; }
        int numThreads() const { return _numThreads; }
        void minDepSize(size_t s) { _minDepSize = s; }
        size_t minDepSize() const { return _minDepSize; }
        void seed(int s);
        void setRandObject(std::tr1::mt19937 & r);
        void depInfo(DepInfo & (* f) (WD &)) { _depInfo = f; }
        void init();
        bool initialized() { return _init; }
        Lock & getLock() { return _initLock; }
        ~DepMethod();
        int chooseSocket(WD * wd);
        void updateSocket(WD * wd, int socket, bool final=false); //, int physnode=0);
        std::tr1::mt19937 & randObject();

#ifdef LOCALITY_USE_NUMA
        void forceSocket( WD * wd, int socket, int physnode );
#endif

      private:
        int _selectSocket() const;
      };

#ifdef NANOS_INSTRUMENTATION_ENABLED
      Lock DepMethod::_generalLock;
      bool DepMethod::_generalInit;
      nanos_event_key_t DepMethod::_movePagesEvent;
#endif

      inline void DepMethod::init()
      {
        assert(_rand != NULL and _numSockets > 0);

        if (sys.getDefaultDependenciesManager() != "cregions") {
          fatal0("The locality-aware schedulers need the 'cregions' dependency manager."
                 << std::endl
                 << "Please set NX_DEPS='cregions' or add '--deps=regions' to NX_ARGS.");
        }

#ifdef NANOS_INSTRUMENTATION_ENABLED
        if (not _generalInit) {
          SyncLockBlock generalLock(_generalLock);
          if (not _generalInit) {
            _movePagesEvent = sys.getInstrumentation()->getInstrumentationDictionary()->registerEventKey( "dep-movepages", "DEP number of pages moved", /*abort*/ true, /*level*/ EVENT_ENABLED, /*stacked*/ false);
            _generalInit = true;
          }
        }
#endif
      }

      inline void DepMethod::seed(int s)
      {
        if (_deleteRand and _rand != NULL) {
          delete _rand;
          _rand = NULL;
        }

        _rand = new std::tr1::mt19937(s);
        _deleteRand = true;
      }

      inline void DepMethod::setRandObject(std::tr1::mt19937 & r)
      {
        if (_deleteRand and _rand != NULL) {
          delete _rand;
        }
        _deleteRand = false;
        _rand = &r;
      }

      inline DepMethod::~DepMethod()
      {
        if (_deleteRand and _rand != NULL) {
          delete _rand;
        }
      }

      inline int DepMethod::_selectSocket() const
      {
        int socket = 0;
        switch (_nextMode) {
        case DepMethod::RANDOM:
          socket = std::tr1::uniform_int<int>()(*_rand, _numSockets);
          break;
        case DepMethod::SOCKET_CYCLIC:
          socket = _nextSocket.value();
          _nextSocket = (_nextSocket.value() + 1)%_numSockets;
          break;
        case DepMethod::CPU_CYCLIC:
          socket = _nextSocket.value();
          _nextSocket = (_nextSocket.value() + 1)%_numThreads;
          socket = -socket - 1;
          break;
        default:
          fatal0( "Chosen socket selection mode is not implemented" );
        }

        return socket;
      }

      inline int DepMethod::chooseSocket(WD * wd)
      {
        if (_depInfo != NULL and _depInfo(*wd).queueNum < 0)
          return _depInfo(*wd).queueNum;
        
        DOSubmit * doS = wd->getDOSubmit();
        if (doS == NULL) {
          return _selectSocket();
        }

        std::vector<unsigned long long> _helper(_numSockets + 1, 0);
      
        const DependableObject::TargetVector &rt = doS->getReadTargets();
        for (_tvIt it = rt.begin(); it != rt.end(); ++it) {
          const void *dep = (*it)->getAddress();
          int currentSocket = _dependencySocket[dep];
          size_t size = (static_cast<DepsRegion *>(*it))->getSize();
          if (size > _minDepSize)
            _helper[currentSocket] += size;
        }

        const DependableObject::TargetVector &wt = doS->getWrittenTargets();
        for (_tvIt it = wt.begin(); it != wt.end(); ++it) {
          const void *dep = (*it)->getAddress();
          int currentSocket = _dependencySocket[dep];
          size_t size = (static_cast<DepsRegion *>(*it))->getSize();
          if (size > _minDepSize)
            _helper[currentSocket] += size;
        }

        int count = 0;
        unsigned long long best = 0;
        int argbest = 0;
        for (int i = 0; i <= _numSockets; ++i) {
          unsigned long long current = _helper[i];
          if (count == 0 or current > best) {
            best = current;
            argbest = i;
            count = 1;
          }
          else if (current == best) {
            ++count;
            std::tr1::uniform_int<int> gen;
            if (gen(*_rand, count) == 0)
              argbest = i;
          }
        }

        if (argbest == 0)
          argbest = _selectSocket();
        else
          --argbest;

        return argbest;
      }

#ifdef LOCALITY_USE_NUMA
      void DepMethod::forceSocket( WD * wd, int socket, int physnode )
      {
        DOSubmit *doS = wd->getDOSubmit();
        if (doS == NULL) {
          return;
        }

        socket = socket + 1;
        
        const DependableObject::TargetVector &rt = doS->getReadTargets();
        for (_tvIt it = rt.begin(); it != rt.end(); ++it) {
          const void *dep = (*it)->getAddress();
          _dependencySocket[dep] = socket;
        }

        const DependableObject::TargetVector &wt = doS->getWrittenTargets();
        std::vector<void *> move_list;
        size_t page_size = sysconf(_SC_PAGESIZE);
        for (_tvIt it = wt.begin(); it != wt.end(); ++it) {
          const char *first = static_cast<const char *>((*it)->getAddress());
          debug("Current socket is " << int(_dependencySocket[first]) << ", new is " << socket);
          if (_dependencySocket[first].isFinal()
              and int(_dependencySocket[first]) != socket) {
            size_t size = (static_cast<DepsRegion *>(*it))->getSize();
            const char *last = first + size;
            for (const char * dep(reinterpret_cast<const char *>(reinterpret_cast<size_t>(first) & ~(page_size-1)));
                 dep < last; dep += page_size) {
              move_list.push_back(reinterpret_cast<void *>(reinterpret_cast<size_t>(dep)));
            }
          }
          _dependencySocket[reinterpret_cast<const void *>(first)].force(socket);
        }
	      
        if (not move_list.empty()) {
          RAISE_EVENT(_movePagesEvent, move_list.size());
          std::vector<int> node_list(move_list.size(), physnode);
          std::vector<int> status_list(move_list.size());
          long status = move_pages(0,
                                   move_list.size(),
                                   &move_list[0],
                                   &node_list[0],
                                   &status_list[0],
                                   MPOL_MF_MOVE);
          debug("Moving " << move_list.size() << " pages!");
          CLOSE_EVENT(_movePagesEvent);
          if (status != 0) {
            char buf[256];
            int myerrno = errno;
            char * errmsg = strerror_r(myerrno, buf, sizeof(buf));
            warning( "Error moving pages to " << physnode << ": " << errmsg );
          }
        }
        else {
          debug( "No pages to move" );
        }
      }
#endif


      void DepMethod::updateSocket( WD * wd, int socket, bool final )
      {
        DOSubmit *doS = wd->getDOSubmit();
        if (doS == NULL) {
          return;
        }

        socket = socket + 1;
        if (not final)
          socket = -socket;
        
        const DependableObject::TargetVector &rt = doS->getReadTargets();
        for (_tvIt it = rt.begin(); it != rt.end(); ++it) {
          const void *dep = (*it)->getAddress();
          _dependencySocket[dep] = socket;
        }

        const DependableObject::TargetVector &wt = doS->getWrittenTargets();
        for (_tvIt it = wt.begin(); it != wt.end(); ++it) {
          const void *dep = (*it)->getAddress();
          _dependencySocket[dep] = socket;
        }
      }
      
    }
  }
}

#undef RAISE_EVENT
#undef CLOSE_EVENT

#endif
