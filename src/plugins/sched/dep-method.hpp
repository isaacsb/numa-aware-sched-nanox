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

namespace nanos {
  namespace ext {
    namespace locsched {
      class DepMethod {
      private:
        typedef DependableObject::TargetVector::const_iterator _tvIt;
        typedef DependableObject::DependableObjectVector::const_iterator _dovIt;

        bool _init;
        int _numSockets;
        size_t _minDepSize;
        bool _deleteRand;
        mutable std::tr1::mt19937 * _rand;
        HashMap< const void *, DepSocket > _dependencySocket;

        Lock _initLock;
        
      public:
        DepMethod() : _init(false), _numSockets(0), _minDepSize(0), _rand(NULL) {}
        void numSockets(int n) { _numSockets = n; }
        int numSockets() const { return _numSockets; }
        void minDepSize(size_t s) { _minDepSize = s; }
        size_t minDepSize() const { return _minDepSize; }
        void seed(int s);
        void setRandObject(std::tr1::mt19937 & r);
        void init();
        bool initialized() { return _init; }
        Lock & getLock() { return _initLock; }
        ~DepMethod();
        int chooseSocket(WD * wd);
        void updateSocket(WD * wd, int socket, bool final=false); //, int physnode=0);
        std::tr1::mt19937 & randObject();

      private:
        int _randomSocket() const;
      };
      

      inline void DepMethod::init()
      {
        assert(_rand != NULL and _numSockets > 0);

        if (sys.getDefaultDependenciesManager() != "cregions") {
          fatal0("The locality-aware schedulers need the 'cregions' dependency manager."
                 << std::endl
                 << "Please set NX_DEPS='cregions' or add '--deps=regions' to NX_ARGS.");
        }
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

      inline int DepMethod::_randomSocket() const
      {
        return std::tr1::uniform_int<int>()(*_rand, _numSockets);
      }

      inline int DepMethod::chooseSocket(WD * wd)
      {
        DOSubmit * doS = wd->getDOSubmit();
        if (doS == NULL) {
          return _randomSocket();
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
          argbest = _randomSocket();
        else
          --argbest;

        return argbest;
      }


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

#endif
