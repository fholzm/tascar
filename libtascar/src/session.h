/* License (GPL)
 *
 * Copyright (C) 2014,2015,2016,2017,2018  Giso Grimm
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License as
 * published by the Free Software Foundation; version 2 of the
 * License.
 *
 * This program is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA
 * 02110-1301, USA.
 */
#ifndef SESSION_H
#define SESSION_H

#include "jackrender.h"
#include "session_reader.h"
#include <set>

namespace TASCAR {

  class session_t;

  class module_cfg_t {
  public:
    module_cfg_t(xmlpp::Element* xmlsrc, TASCAR::session_t* session_ );
    TASCAR::session_t* session;
    xmlpp::Element* xmlsrc;
  };

  class module_base_t : public xml_element_t, public audiostates_t {
  public:
    module_base_t( const module_cfg_t& cfg );
    virtual ~module_base_t();
    /**
       \brief Update geometry etc on each processing cycle in the session processing thread.
       \callgraph
       \callergraph

       This method will be called after scene geometry update and
       before acoustic model update and audio rendering.

       \param frame Transport time (in samples).
       \param running Transport running (true) or stopped (false).
     */
    virtual void update(uint32_t frame,bool running);
  protected:
    TASCAR::session_t* session;
  };

  class module_t : public module_base_t {
  public:
    module_t( const TASCAR::module_cfg_t& cfg );
    virtual ~module_t();
    void prepare( chunk_cfg_t& );
    void release();
    void update(uint32_t frame,bool running);
    virtual void validate_attributes(std::string&) const;
  private:
    std::string name;
    void* lib;
  public:
    TASCAR::module_base_t* libdata;
  private:
    bool is_configured;
  };

  class connection_t : public TASCAR::xml_element_t {
  public:
    connection_t(xmlpp::Element*);
    std::string src;
    std::string dest;
  };

  class range_t : public scene_node_base_t {
  public:
    range_t(xmlpp::Element* e);
    std::string name;
    double start;
    double end;
  };

  class named_object_t {
  public:
    named_object_t(TASCAR::Scene::object_t* o,const std::string& n) : obj(o),name(n){};
    TASCAR::Scene::object_t* obj; //< pointer to object
    std::string name; //< name of object
  };

  class session_oscvars_t : public TASCAR::xml_element_t {
  public:
    session_oscvars_t( xmlpp::Element* src );
    std::string name;
    std::string srv_port;
    std::string srv_addr;
    std::string srv_proto;
  };

  class session_core_t : public TASCAR::tsc_reader_t {
  public:
    session_core_t();
    session_core_t(const std::string& filename_or_data,load_type_t t,const std::string& path);
    // configuration variables:
    //std::string name;
    double duration;
    bool loop;
    double levelmeter_tc;
    TASCAR::levelmeter_t::weight_t levelmeter_weight;
    std::string levelmeter_mode;
    double levelmeter_min;
    double levelmeter_range;
    double requiresrate;
    double warnsrate;
    int32_t requirefragsize;
    int32_t warnfragsize;
  };

  class session_t : public session_core_t, public session_oscvars_t, public jackc_transport_t, public TASCAR::osc_server_t {
  public:
    session_t();
    session_t(const std::string& filename_or_data,load_type_t t,const std::string& path);
  private:
    session_t(const session_t& src);
  public:
    virtual ~session_t();
    void add_scene(xmlpp::Element* src=NULL);
    void add_range(xmlpp::Element*);
    void add_connection(xmlpp::Element*);
    void add_module(xmlpp::Element*);
    void start();
    void stop();
    void run(bool &b_quit);
    double get_duration() const { return duration; };
    uint32_t get_active_pointsources() const;
    uint32_t get_total_pointsources() const;
    uint32_t get_active_diffuse_sound_fields() const;
    uint32_t get_total_diffuse_sound_fields() const;
    std::vector<TASCAR::named_object_t> find_objects(const std::string& pattern);
    std::vector<TASCAR::Scene::audio_port_t*> find_audio_ports(const std::vector<std::string>& pattern);
    std::vector<TASCAR::scene_render_rt_t*> scenes;
    std::vector<TASCAR::range_t*> ranges;
    std::vector<TASCAR::connection_t*> connections;
    std::vector<TASCAR::module_t*> modules;
    std::vector<std::string> get_render_output_ports() const;
    virtual int process(jack_nframes_t nframes,const std::vector<float*>& inBuffer,const std::vector<float*>& outBuffer,uint32_t tp_frame, bool tp_rolling);
    void unload_modules();
    bool lock_vars();
    void unlock_vars();
    bool trylock_vars();
    bool is_running() { return started_; };
    virtual void validate_attributes(std::string &) const;
  protected:
    // derived variables:
    std::string session_path;
  private:
    void add_transport_methods();
    void read_xml();
    double period_time;
    bool started_;
    pthread_mutex_t mtx;
  };

  /// Control 'actors' in a scene
  class actor_module_t : public module_base_t {
  public:
    actor_module_t( const TASCAR::module_cfg_t& cfg, bool fail_on_empty=false );
    virtual ~actor_module_t();
    /**@brief Set delta location of all actors
       @param l new delta location
       @param b_local Apply in local coordinates (true) or in global coordinates (false)
     */
    void set_location(const TASCAR::pos_t& l, bool b_local = false );
    void set_orientation(const TASCAR::zyx_euler_t& o );
    void set_transformation( const TASCAR::c6dof_t& tf, bool b_local = false );
    void add_location(const TASCAR::pos_t& l, bool b_local );
    void add_orientation(const TASCAR::zyx_euler_t& o );
    void add_transformation( const TASCAR::c6dof_t& tf, bool b_local = false );
  protected:
    /**
       \brief Actor name pattern
     */
    std::string actor;
    /**
       \brief List of matching actor objects
     */
    std::vector<TASCAR::named_object_t> obj;
  };

}

#define REGISTER_MODULE(x) TASCAR_PLUGIN( module_base_t, const module_cfg_t&, x )

#endif

/*
 * Local Variables:
 * mode: c++
 * c-basic-offset: 2
 * indent-tabs-mode: nil
 * compile-command: "make -C .."
 * End:
 */
