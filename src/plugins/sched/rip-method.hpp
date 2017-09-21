#ifndef _NANOX_LOCALITY_RIP_METHOD
#define _NANOX_LOCALITY_RIP_METHOD

#include "workdescriptor.hpp"
#include "dependableobject.hpp"
#include "config.hpp"

#include "schedule.hpp"
#include "depsregion.hpp"

#ifdef USE_METAPART
extern "C" {
#include "kgggp.h"
#include "ext/scotch.h"
}
#else
#include "scotch.h"
#endif

#include "locality-common.hpp"

#include <map>
#include <vector>

#include <tr1/random>
#include <cassert>

namespace nanos {
  namespace ext {
    namespace locsched {
      class RipMethod
      {
      public:
        struct RipInfo
        {
          int id;
          RipInfo() : id(0) {}
        };
        
      private:
        typedef DependableObject::TargetVector::const_iterator _tvIt;
        typedef DependableObject::DependableObjectVector::const_iterator _dovIt;

        bool _init;
        int _numSockets;
        size_t _minDepSize;
        double _imbalance;
        bool _oldDep;
        // bool _migrate
        bool _deleteRand;
        mutable std::tr1::mt19937 * _rand;

        RipInfo & (*_ripInfo) (WD &);

        SchedulePolicy * _parentSched;
        void (* _notifyScheduler) (SchedulePolicy *);

        std::map<const void *, std::vector<int> > _antipred;
        std::map<const void *, int> _oldPred;
        std::map<const void *, int> _pred;
        std::map<int, std::map<int, size_t> > _deps;
        std::map<int, std::map<int, size_t> >_invdeps;
        Lock _graphLock;

        SCOTCH_Arch _arch;
        SCOTCH_Strat _firstStrat;
        SCOTCH_Strat _windowStrat;

        std::vector<SCOTCH_Num> _taskSocket;
        mutable RWLock _tsLock;
        
        Atomic<int> _lastPartitioned;
        Atomic<int> _lastVirtId;

        Lock _initLock;

      public:
        RipMethod();
        void archInfo(const std::vector<SocketInfo> & s);
        int numSockets() const { return _numSockets; }
        void minDepSize(size_t s) { _minDepSize = s; }
        size_t minDepSize() const { return _minDepSize; }
        void imbalance(double ib) { _imbalance = ib; }
        double imbalance() { return _imbalance; }
        template<typename T>
        void afterPartition( T notifySched ) { _notifyScheduler = notifySched; }
        void seed(int s);
        void seedPartitioner(int s) { SCOTCH_randomSeed(s); }
        void setRandObject(std::tr1::mt19937 & r);
        void ripInfo(RipInfo & (* f) (WD &)) { _ripInfo = f; }
        void parentSched(SchedulePolicy * parentSched) { _parentSched = parentSched; }
        void init();
        bool initialized() { return _init; }
        Lock & getLock() { return _initLock; }
        ~RipMethod();
        
        std::tr1::mt19937 & randObject();

        int chooseSocket(WD * wd) const;
        int chooseSocketUnprotected(WD * wd) const;
        void acquireChooseSocket() const { _tsLock.acquireRead(); }
        void releaseChooseSocket() const { _tsLock.releaseRead(); }
        void updateSocket(WD * wd, int socket, bool final=false); // , int physnode=0);
        int addTask(WD & wd);
        void partition(int first, int last, int next);
        int lastAddedTask() const { return _lastVirtId.value(); }

      private:
        class _Graph {
        public:
          std::vector<SCOTCH_Num> verttab;
          std::vector<SCOTCH_Num> edgetab;
          std::vector<SCOTCH_Num> edlotab;
          SCOTCH_Num num_vertices;
          SCOTCH_Num first;
          SCOTCH_Num last;
          SCOTCH_Num baseval;
        private:
          SCOTCH_Graph _g;
        public:
          _Graph() : baseval(0) { SCOTCH_graphInit(&_g); }
          ~_Graph() { SCOTCH_graphExit(&_g); }
          operator SCOTCH_Graph *();
        };
        void _initFirstStrat();
        void _initWindowStrat();
        void _buildGraph(_Graph & g, int first, int last, int next);
        void _doPartition(_Graph & g);

#ifdef USE_METAPART
        std::string _getStratString();
#endif
      };

      inline RipMethod::_Graph::operator SCOTCH_Graph *()
      {
        SCOTCH_graphExit(&_g);
        SCOTCH_graphInit(&_g);
        SCOTCH_graphBuild(&_g,
                          baseval, // base value
                          verttab.size() - 1,
                          &verttab[0],
                          &verttab[1],
                          NULL, // vertex loads
                          NULL, // vertex labels
                          edgetab.size(),
                          &edgetab[0],
                          &edlotab[0]);

#ifdef NANOS_DEBUG_ENABLED
        // if (SCOTCH_graphCheck(&_g) == 0) {
        //   warning( "Graph is ok" );
        // }
#endif

        return &_g;
      }
      
      inline RipMethod::RipMethod()
        : _init(false),
          _numSockets(0),
          _minDepSize(0),
          // _migrate(false),
          _oldDep(false),
          _deleteRand(false),
          _rand(NULL),
          _ripInfo(NULL)
      {}

      inline RipMethod::~RipMethod()
      {
        if (_deleteRand and _rand != NULL) {
          delete _rand;
        }
      }
      
      void RipMethod::init()
      {
        assert(_rand != NULL and _numSockets > 0 and _ripInfo != NULL);

        if (sys.getDefaultDependenciesManager() != "cregions") {
          fatal0("The locality-aware schedulers need the 'cregions' dependency manager."
                 << std::endl
                 << "Please set NX_DEPS='cregions' or add '--deps=regions' to NX_ARGS.");
        }

        _initFirstStrat();
        _initWindowStrat();
      }

      void RipMethod::_initFirstStrat()
      {
        SCOTCH_stratInit(&_firstStrat);
    	SCOTCH_stratGraphMapBuild(&_firstStrat,
    				  SCOTCH_STRATDEFAULT|SCOTCH_STRATQUALITY|SCOTCH_STRATRECURSIVE,
    				  _numSockets,
    				  _imbalance);
      }

      void RipMethod::_initWindowStrat()
      {
        SCOTCH_stratInit(&_windowStrat);
#ifdef USE_METAPART
        std::string mystrat = _getStratString();
        SCOTCH_stratGraphMap(&_windowStrat,
                             mystrat.c_str());
#else
        SCOTCH_stratGraphMapBuild(&_windowStrat,
    				  SCOTCH_STRATDEFAULT|SCOTCH_STRATREMAP|SCOTCH_STRATQUALITY,
    				  _numSockets,
    				  _imbalance);
#endif
      }

      inline void RipMethod::archInfo(const std::vector<SocketInfo> & s)
      {
        SCOTCH_archInit(&_arch);
        _numSockets = s.size();

        std::vector<SCOTCH_Num> arch_verttab, arch_vendtab;
        std::vector<SCOTCH_Num> arch_capacities;
        std::vector<SCOTCH_Num> arch_edges, arch_distances;

        arch_verttab.reserve(_numSockets);
        arch_vendtab.reserve(_numSockets);
        arch_capacities.reserve(_numSockets);
        arch_edges.reserve(_numSockets*(_numSockets - 1));
        arch_distances.reserve(_numSockets*(_numSockets - 1));

        for (int i = 0; i < _numSockets; ++i) {
          arch_capacities.push_back(s[i].weight);
          arch_verttab.push_back(arch_edges.size());
          for (int j = 0; j < _numSockets; ++j) {
            if (i != j) {
              arch_edges.push_back(j);
              arch_distances.push_back(s[i].distances[j]);
            }
          }
          arch_vendtab.push_back(arch_edges.size());
        }

        SCOTCH_Graph arch_graph;
        SCOTCH_graphInit(&arch_graph);
        SCOTCH_graphBuild(&arch_graph,
                          0, // baseval
                          _numSockets,
                          &arch_verttab[0],
                          &arch_vendtab[0],
                          &arch_capacities[0],
                          NULL,
                          arch_edges.size(),
                          &arch_edges[0],
                          &arch_distances[0]);

        int status = SCOTCH_graphCheck(&arch_graph);
        if (status != 0) {
          fatal0( "Problem building the graph of the architecture" );
        }
        
        SCOTCH_stratInit(&_firstStrat);
        status = SCOTCH_archBuild(&_arch,
                                  &arch_graph,
                                  0,
                                  NULL,
                                  &_firstStrat);
        
        if (status != 0) {
          fatal0( "Problem building the architecture from the graph" );
        }
        
        SCOTCH_stratExit(&_firstStrat);
        SCOTCH_graphExit(&arch_graph);
      }

      inline void RipMethod::seed(int s)
      {
        if (_deleteRand and _rand != NULL) {
          delete _rand;
          _rand = NULL;
        }

        _rand = new std::tr1::mt19937(s);
        _deleteRand = true;
      }

      inline void RipMethod::setRandObject(std::tr1::mt19937 & r)
      {
        if (_deleteRand and _rand != NULL) {
          delete _rand;
        }
        _deleteRand = false;
        _rand = &r;
      }

      inline int RipMethod::chooseSocketUnprotected(WD * wd) const
      {
        if (_ripInfo(*wd).id == 0)
          return -2;
        
        int n = _taskSocket.size();
        if (_ripInfo(*wd).id > 0 and _ripInfo(*wd).id < n)
          return _taskSocket[_ripInfo(*wd).id];
        
        return -1;
      }

      inline int RipMethod::chooseSocket(WD * wd) const
      {
        if (_ripInfo(*wd).id == 0)
          return -2;
        
        acquireChooseSocket();
        int result = chooseSocketUnprotected(wd);
        releaseChooseSocket();
        return result;
      }

      inline void RipMethod::updateSocket(WD * wd, int socket, bool final)
      {
        if (final) {
          _tsLock.acquireWrite();
          _taskSocket[_ripInfo(*wd).id] = socket;
          _tsLock.releaseWrite();
        }
      }

      inline int RipMethod::addTask(WD & wd)
      {
        if (_ripInfo(wd).id != 0)
          return _ripInfo(wd).id;

        int virtId = ++_lastVirtId;
        _ripInfo(wd).id = virtId;

        _graphLock.acquire();
        // Add missing elements to the basic graph structure
	if (_deps.find(virtId) == _deps.end()) {
          int n = _deps.size();
	  for (int i = n; i <= virtId; ++i) {
	    _deps[i].size(); // ensure vertex creation
	    _invdeps[i].size();
	  }
	  /// _lastWd = virtId;
	}
        
        DOSubmit *doS = wd.getDOSubmit();
        const DependableObject::TargetVector &rt = doS->getReadTargets();
	for (_tvIt it = rt.begin(); it != rt.end(); ++it) {
	  const void *dep = (*it)->getAddress();
	  size_t size = (static_cast<DepsRegion *>(*it))->getSize();
          if (size <= _minDepSize)
            continue;
          
          int o_parent = _oldPred[dep];
          if (_oldDep and o_parent != 0) { // If 0, nobody has written to it yet
            if (_deps[o_parent].find(virtId) == _deps[o_parent].end()) {
              _deps[o_parent].insert(std::make_pair(virtId, size));
              _invdeps[virtId].insert(std::make_pair(o_parent, size));
            }
          }

          int parent = _pred[dep];
          if (parent != 0 and parent != o_parent) { // If 0, nobody has written to it yet
            if (_deps[parent].find(virtId) == _deps[parent].end()) {
              _deps[parent].insert(std::make_pair(virtId, size));
              _invdeps[virtId].insert(std::make_pair(parent, size));
            }
          }

          _antipred[dep].push_back(virtId); // For antidependencies
	}

	const DependableObject::TargetVector &wt = doS->getWrittenTargets();
	for (_tvIt it = wt.begin(); it != wt.end(); ++it) {
	  const void *dep = (*it)->getAddress();
	  size_t size = (static_cast<DepsRegion *>(*it))->getSize();
          if (size <= _minDepSize)
            continue;
          
	  int o_parent = _oldPred[dep];
	  if (_oldDep and o_parent != 0 and o_parent != virtId) {
	    if (_deps[o_parent].find(virtId) == _deps[o_parent].end()) {
	      _deps[o_parent].insert(std::make_pair(virtId, size));
	      _invdeps[virtId].insert(std::make_pair(o_parent, size));
	    }
	  }
	  
	  int parent = _pred[dep];
	  if (parent != 0 and parent != virtId and parent != o_parent) { // If -1, nobody has written to it yet
	    if (_deps[parent].find(virtId) == _deps[parent].end()) {
	      _deps[parent].insert(std::make_pair(virtId, size));
	      _invdeps[virtId].insert(std::make_pair(parent, size));
	    }
	  }

	  std::vector<int> &v = _antipred[dep];
	  int n = v.size();
	  for (int i = 0; i < n; ++i) {
	    _deps[v[i]].insert(std::make_pair(virtId, size));
	    _invdeps[virtId].insert(std::make_pair(v[i], size));

	  }
	  v.clear();
	  

	  if (_pred[dep] != virtId) {
	    _oldPred[dep] = _pred[dep];
	  }
	  _pred[dep] = virtId;
	}
        
	_graphLock.release();

        return virtId;
      }

      inline void RipMethod::partition(int first, int last, int nextFirst)
      {
        _Graph g;
        _buildGraph(g, first, last, nextFirst);
        _doPartition(g);

        _notifyScheduler(_parentSched);
      }

      inline void RipMethod::_buildGraph(_Graph & g, int first, int last, int next)
      {
        g.num_vertices = last - first + 1;
        g.verttab.reserve(g.num_vertices + 1);
        g.verttab.push_back(g.baseval);

        g.first = first;
        g.last = last;

        _graphLock.acquire();
        for (int i = first; i <= last; ++i) {
	  const std::map<int, size_t> &v1 = _deps[i];
	  for (std::map<int, size_t>::const_iterator it = v1.begin();
	       it != v1.end();
	       ++it) {
	    if (it->first >= first and it->first <= last) {
	      g.edgetab.push_back(static_cast<SCOTCH_Num>(it->first - first));
	      g.edlotab.push_back(static_cast<SCOTCH_Num>(std::max(static_cast<SCOTCH_Num>(it->second/1024), (SCOTCH_Num) 1)));
	    }
	  }

	  std::map<int, size_t> &v2 = _invdeps[i];
	  bool prev_old = true;
	  for (std::map<int, size_t>::iterator it = v2.begin();
	       it != v2.end();
	       ++it) {
	    if (it->first >= first and it->first <= last) {
	      g.edgetab.push_back(static_cast<SCOTCH_Num>(it->first - first));
	      g.edlotab.push_back(static_cast<SCOTCH_Num>(std::max(static_cast<SCOTCH_Num>(it->second/1024), (SCOTCH_Num) 1)));
	    }

	    if (prev_old and it->first >= next) {
	      v2.erase(v2.begin(), it);
	      prev_old = false;
	    }
	  }

	  if (i < next) {
	    _deps.erase(_deps.find(i));
	    _invdeps.erase(_invdeps.find(i));
	  }

	  g.verttab.push_back(g.edgetab.size() + g.baseval);
        }

        std::map<const void *, int>::iterator it_p = _pred.begin();
	while (it_p != _pred.end()) {
	  std::map<const void *, int>::iterator next_it = it_p;
	  ++next_it;
	  if (it_p->second < next)
	    _pred.erase(it_p);

	  it_p = next_it;
	}

	it_p = _oldPred.begin();
	while (it_p != _oldPred.end()) {
	  std::map<const void *, int>::iterator next_it = it_p;
	  ++next_it;
	  if (it_p->second < next)
	    _oldPred.erase(it_p);

	  it_p = next_it;
	}

        _graphLock.release();
      }

      void RipMethod::_doPartition(_Graph & g)
      {
        _tsLock.acquireWrite();
        int n = _taskSocket.size();
        if (n <= g.last) {
          _taskSocket.resize(g.last + 1, -1);
        }
        if (g.first >= n) {
          SCOTCH_graphMap(g, // implicit conversion to SCOTCH_Graph *
                          &_arch,
                          &_firstStrat,
                          &_taskSocket[g.first]);
        }
        else { // nothing to propagate...
#ifdef USE_METAPART
          SCOTCH_graphPartFixed(g, // implicit conversion to SCOTCH_Graph *
	  			_numSockets,
	  			&_windowStrat,
	  			&_taskSocket[g.first]);
#else
          SCOTCH_graphMapFixed(g, // implicit conversion to SCOTCH_Graph *
                               &_arch,
                               &_windowStrat,
                               &_taskSocket[g.first]);
#endif
        }
        _tsLock.releaseWrite();
      }

#ifdef USE_METAPART
      std::string RipMethod::_getStratString()
      {
#define MAXSTR 8192
#define NREFPASS 10
#define NINITPASS 4
#define MAXNEGMOVE 300

        int nparts = _numSockets;
        double imbalance = _imbalance;
       
	int gainscheme, greedy, connectivity, useseeds, npass, multilevel;
  
	gainscheme = KGGGP_GAIN_CLASSIC;
	greedy = KGGGP_GREEDY_GLOBAL;
	connectivity = KGGGP_CONNECTIVITY_YES;
	useseeds = KGGGP_USE_SEEDS;
	// npass = NINITPASS;
	npass = NINITPASS;
	multilevel = 0;

	int maxnegmove = MAXNEGMOVE;
	// if(maxnegmove < 0) maxnegmove = INT_MAX;  
  
	char gain = 'c';
	if(gainscheme == KGGGP_GAIN_DIFF) gain = 'd';
	else if(gainscheme == KGGGP_GAIN_HYBRID) gain = 'h';  
  
	char grdy = 'g';
	if(greedy == KGGGP_GREEDY_LOCAL) grdy = 'l';
  
	char conn = 'n';
	if(connectivity == KGGGP_CONNECTIVITY_YES) conn = 'y';
  
	char seed = 'n';
	if(useseeds == KGGGP_USE_SEEDS) seed = 's'; else if(useseeds == KGGGP_USE_BUBBLES) seed = 'b';
  
	char s[MAXSTR];
	snprintf (s, sizeof(s),
		  "g{bal=%lf,gain=%c,seed=%c,greedy=%c,conn=%c,pass=%d}f{bal=%lf,pass=%d,move=%d}", // initial part
		  imbalance, // <- ubfactor
		  gain,      // <- gain scheme
		  seed,      // <- use seeds (or not)
		  grdy,      // <- greedy approach (global or local)
		  conn,      // <- enforce connectivity
		  npass,     // <- nb of KGGGP passes
		  imbalance, // <- ubfactor	    
		  NREFPASS,
                  maxnegmove // <- maxnegmove for refinement (or -1)	    
		  );

	char * sss = s;

	if (multilevel) {
	  char ss[8192];
	  snprintf (ss, sizeof(ss),
		    "m{vert=%d,low=%s,asc=f{bal=%lf,move=%d,pass=%d}}",
		    30*nparts,  // <- max size of coarsest graph
		    s,          // <- KGGGP + FM refinement
		    imbalance,  // <- ubfactor
		    maxnegmove, // <- maxnegmove for refinement (or -1)	    	    
		    NREFPASS    // <- max nb of pass (or -1)
		    );
  
	  sss = ss;
	}

	return std::string(sss);
	
#undef MAXSTR
#undef NREFPASS
#undef NINITPASS
#undef MAXNEGMOVE
      }
#endif
      
    }
  }
}

#endif
