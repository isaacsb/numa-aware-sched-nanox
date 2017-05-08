#include "plugin.hpp"
#include "system.hpp"
#include "instrumentation.hpp"
#include "lock.hpp"
#include <map>
#include <string>
#include <fstream>

namespace nanos {

  class InstrumentationExecMap: public Instrumentation 
  {
#ifndef NANOS_INSTRUMENTATION_ENABLED
  public:
    // constructor
    InstrumentationExecMap( ) : Instrumentation( ) {}
    // destructor
    ~InstrumentationExecMap() {}

    // low-level instrumentation interface (mandatory functions)
    void initialize( void ) {}
    void finalize( void ) {}
    void disable( void ) {}
    void enable( void ) {}
    void addResumeTask( WorkDescriptor &w ) {}
    void addSuspendTask( WorkDescriptor &w, bool last ) {}
    void addEventList ( unsigned int count, Event *events ) {}
    void threadStart( BaseThread &thread ) {}
    void threadFinish ( BaseThread &thread ) {}
#else
  private:

    struct _TaskData
    {
      int numa_node;
      std::string name;
      std::vector< std::pair<const void *, size_t> > in_addr;
      std::vector< std::pair<const void *, size_t> > out_addr;

      _TaskData(int new_numa_node=-1)
	: numa_node(new_numa_node),
	  name("")
      {}

      _TaskData(const _TaskData & o)
	: numa_node(o.numa_node),
	  name(o.name),
	  in_addr(o.in_addr),
	  out_addr(o.out_addr)
      {}

      _TaskData & operator=(const _TaskData & o)
      {
	if (&o != this) {
	  name = o.name;
	  in_addr = o.in_addr;
	  out_addr = o.out_addr;
	  numa_node = o.numa_node;
	}

	return *this;
      }

      _TaskData & operator=(int new_numa_node)
      {
	numa_node = new_numa_node;

	return *this;
      }

      inline bool operator==(int other_numa_node) const
      {
	return numa_node == other_numa_node;
      }

      inline bool operator!=(int other_numa_node) const
      {
	return numa_node != other_numa_node;
      }
    };

    typedef DependableObject::TargetVector::const_iterator _tvIt;
    
    bool _last;
    bool _deps;
    std::string _filename;
    Lock _mapLock;
    std::map<int, _TaskData> _execMap;
    
  public:
    // constructor
    InstrumentationExecMap( bool last,
			    bool deps,
			    std::string filename)
      : Instrumentation( *new InstrumentationContextDisabled() ),
	_last(last),
	_deps(deps),
	_filename(filename)
    {}
    // destructor
    ~InstrumentationExecMap () {}

    // low-level instrumentation interface (mandatory functions)
    void initialize( void ) {}
    void finalize( void )
    {
      warning0( "Saving execution map to file " << _filename );
      std::ofstream mapFile;
      mapFile.open(_filename.c_str());
      if ( not mapFile.good() ) {
	warning0( "Could not save execution map!!" );
      }
      else {
	mapFile << sys.getNumNumaNodes() << std::endl;
	mapFile << _execMap.size() << std::endl;

	int i = 0;
	for (std::map<int, _TaskData>::const_iterator it = _execMap.begin();
	     it != _execMap.end();
	     ++it, ++i) {
	  mapFile << i << ' ' << it->second.numa_node;

	  if (_deps) {
	    mapFile << ' ' << it->second.in_addr.size();
	    for (std::vector< std::pair<const void *, size_t> >::const_iterator d = it->second.in_addr.begin();
		 d != it->second.in_addr.end();
		 ++d) {
	      mapFile << ' ' << d->first << ' ' << d->second;
	    }

	    mapFile << ' ' << it->second.out_addr.size();
	    for (std::vector< std::pair<const void *, size_t> >::const_iterator d = it->second.out_addr.begin();
		 d != it->second.out_addr.end();
		 ++d) {
	      mapFile << ' ' << d->first << ' ' << d->second;
	    }
	  }

	  mapFile << ' ' << it->second.name;

	  mapFile << std::endl;
	}

	mapFile.close();
      }
    }
    void disable( void ) {}
    void enable( void ) {}
    void addResumeTask( WorkDescriptor &w )
    {
      if (w.isImplicit() or w.getId() <= sys.getNumThreads() + 1) // or not last)
	return;

      unsigned node = myThread->runningOn()->getNumaNode();
      int vNode = static_cast<int>( sys.getVirtualNUMANode( node ) );
      
      SyncLockBlock b(_mapLock);
      
      std::map<int, _TaskData>::iterator it = _execMap.find(w.getId());
      if (it == _execMap.end()) {
	_TaskData & d = _execMap[w.getId()] = vNode;

	d.name = w.getDescription();
	
	DOSubmit * doS = w.getDOSubmit();
	if (_deps and doS != NULL) {
	  const DependableObject::TargetVector &rt = doS->getReadTargets();
	  for (_tvIt it2 = rt.begin(); it2 != rt.end(); ++it2) {
	    d.in_addr.push_back(std::make_pair((*it2)->getAddress(), 1 /*(*it2)->size()*/));
	  }
	  
	  const DependableObject::TargetVector &wt = doS->getWrittenTargets();
	  for (_tvIt it2 = wt.begin(); it2 != wt.end(); ++it2) {
	    d.out_addr.push_back(std::make_pair((*it2)->getAddress(), 1/* (*it2)->size()*/));
	  }
	}
      }
      else if (it->second != vNode) {
	warning( "Work descriptor " << w.getId() << " has changed node from " << it->second.numa_node << " to " << vNode );
	if (_last) {
	  it->second = vNode;
	}
      }
    }
    
    void addSuspendTask( WorkDescriptor &w, bool last ) {}
    void addEventList ( unsigned int count, Event *events ) {}
    void threadStart( BaseThread &thread ) {}
    void threadFinish ( BaseThread &thread ) {}
#endif
  };

  namespace ext {

    class InstrumentationExecMapPlugin : public Plugin {
#ifdef NANOS_INSTRUMENTATION_ENABLED
      bool _last;
      bool _deps;
      std::string _filename;
#endif
    public:
      InstrumentationExecMapPlugin ()
	: Plugin("Save execution map for plotting and precalculated schedulers.",1)
      {
#ifdef NANOS_INSTRUMENTATION_ENABLED
	_last = false;
	_deps = false;
	_filename = "execution.map";
#endif
      }
      ~InstrumentationExecMapPlugin () {}

      void config( Config &cfg )
      {
#ifdef NANOS_INSTRUMENTATION_ENABLED
	cfg.setOptionsSection( "Execution map instrumentation",
			       "Execution map instrumentation module" );

	cfg.registerConfigOption( "execmap-use-last", NEW Config::FlagOption(_last), "Save last socket instead of first socket in map (default: false)" );
	cfg.registerArgOption( "execmap-use-last", "execmap-use-last" );

	cfg.registerConfigOption( "execmap-write-deps", NEW Config::FlagOption(_deps), "" );
	cfg.registerArgOption( "execmap-write-deps", "execmap-write-deps" );

	cfg.registerConfigOption( "execmap-output-file", NEW Config::StringVar( _filename ), "Output filename for execution map (defualt: execution.map)" );
	cfg.registerArgOption( "execmap-output-file", "execmap-output-file" );
#endif
      }

      void init ()
      {
	sys.setInstrumentation(
			       new InstrumentationExecMap(
#ifdef NANOS_INSTRUMENTATION_ENABLED
							  _last,
							  _deps,
							  _filename
#endif
							  ) );
      }
    };

  } // namespace ext

} // namespace nanos

DECLARE_PLUGIN("instrumentation-execution_map",nanos::ext::InstrumentationExecMapPlugin);
