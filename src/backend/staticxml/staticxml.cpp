#include "cgimap/backend/staticxml/staticxml.hpp"
#include "cgimap/backend.hpp"
#include "cgimap/output_formatter.hpp"
#include "cgimap/api06/id_version.hpp"

#include <libxml/parser.h>

#include <boost/make_shared.hpp>
#include <boost/format.hpp>
#include <boost/lexical_cast.hpp>
#include <boost/foreach.hpp>
#include <sstream>

namespace po = boost::program_options;
using boost::shared_ptr;
using std::string;
using api06::id_version;

#define CACHE_SIZE 1000

namespace {

// needed to get boost::lexical_cast<bool>(string) to work.
// see:
// http://stackoverflow.com/questions/4452136/how-do-i-use-boostlexical-cast-and-stdboolalpha-i-e-boostlexical-cast-b
struct bool_alpha {
  bool data;
  bool_alpha() {}
  bool_alpha(bool data) : data(data) {}
  operator bool() const { return data; }
  friend std::ostream &operator<<(std::ostream &out, bool_alpha b) {
    out << std::boolalpha << b.data;
    return out;
  }
  friend std::istream &operator>>(std::istream &in, bool_alpha &b) {
    in >> std::boolalpha >> b.data;
    return in;
  }
};

struct node {
  element_info m_info;
  double m_lon, m_lat;
  tags_t m_tags;
};

struct way {
  element_info m_info;
  nodes_t m_nodes;
  tags_t m_tags;
};

struct relation {
  element_info m_info;
  members_t m_members;
  tags_t m_tags;
};

struct database {
  std::map<id_version, node> m_nodes;
  std::map<id_version, way> m_ways;
  std::map<id_version, relation> m_relations;
};

template <typename T>
T get_attribute(const char *name, size_t len, const xmlChar **attributes) {
  while (*attributes != NULL) {
    if (strncmp(((const char *)(*attributes++)), name, len) == 0) {
      return boost::lexical_cast<T>((const char *)(*attributes));
    }
    ++attributes;
  }
  throw std::runtime_error(
      (boost::format("Unable to find attribute %1%.") % name).str());
}

template <typename T>
boost::optional<T> opt_attribute(const char *name, size_t len,
                                 const xmlChar **attributes) {
  while (*attributes != NULL) {
    if (strncmp(((const char *)(*attributes++)), name, len) == 0) {
      return boost::lexical_cast<T>((const char *)(*attributes));
    }
    ++attributes;
  }
  return boost::none;
}

void parse_info(element_info &info, const xmlChar **attributes) {
  info.id = get_attribute<osm_nwr_id_t>("id", 2, attributes);
  info.version = get_attribute<osm_nwr_id_t>("version", 8, attributes);
  info.changeset = get_attribute<osm_changeset_id_t>("changeset", 2, attributes);
  info.timestamp = get_attribute<std::string>("timestamp", 10, attributes);
  info.uid = opt_attribute<osm_user_id_t>("uid", 4, attributes);
  info.display_name = opt_attribute<std::string>("user", 5, attributes);
  info.visible = get_attribute<bool_alpha>("visible", 8, attributes);
}

struct xml_parser {
  xml_parser(database *db) : m_db(db) {}

  static void start_element(void *ctx, const xmlChar *name,
                            const xmlChar **attributes) {
    xml_parser *parser = static_cast<xml_parser *>(ctx);

    if (strncmp((const char *)name, "node", 5) == 0) {
      node n;
      parse_info(n.m_info, attributes);
      n.m_lon = get_attribute<double>("lon", 4, attributes);
      n.m_lat = get_attribute<double>("lat", 4, attributes);
      id_version idv(n.m_info.id, n.m_info.version);
      std::pair<std::map<id_version, node>::iterator, bool> status =
          parser->m_db->m_nodes.insert(std::make_pair(idv, n));
      parser->m_cur_node = &(status.first->second);
      parser->m_cur_tags = &(parser->m_cur_node->m_tags);
      parser->m_cur_way = NULL;
      parser->m_cur_rel = NULL;

    } else if (strncmp((const char *)name, "way", 4) == 0) {
      way w;
      parse_info(w.m_info, attributes);
      id_version idv(w.m_info.id, w.m_info.version);
      std::pair<std::map<id_version, way>::iterator, bool> status =
          parser->m_db->m_ways.insert(std::make_pair(idv, w));
      parser->m_cur_way = &(status.first->second);
      parser->m_cur_tags = &(parser->m_cur_way->m_tags);
      parser->m_cur_node = NULL;
      parser->m_cur_rel = NULL;

    } else if (strncmp((const char *)name, "relation", 9) == 0) {
      relation r;
      parse_info(r.m_info, attributes);
      id_version idv(r.m_info.id, r.m_info.version);
      std::pair<std::map<id_version, relation>::iterator, bool> status =
          parser->m_db->m_relations.insert(std::make_pair(idv, r));
      parser->m_cur_rel = &(status.first->second);
      parser->m_cur_tags = &(parser->m_cur_rel->m_tags);
      parser->m_cur_node = NULL;
      parser->m_cur_way = NULL;

    } else if (strncmp((const char *)name, "tag", 4) == 0) {
      if (parser->m_cur_tags != NULL) {
        std::string k = get_attribute<std::string>("k", 2, attributes);
        std::string v = get_attribute<std::string>("v", 2, attributes);

        parser->m_cur_tags->push_back(std::make_pair(k, v));
      }

    } else if (strncmp((const char *)name, "nd", 3) == 0) {
      if (parser->m_cur_way != NULL) {
        parser->m_cur_way->m_nodes.push_back(
            get_attribute<osm_nwr_id_t>("ref", 4, attributes));
      }

    } else if (strncmp((const char *)name, "member", 7) == 0) {
      if (parser->m_cur_rel != NULL) {
        member_info m;
        std::string member_type =
            get_attribute<std::string>("type", 5, attributes);

        if (member_type == "node") {
          m.type = element_type_node;
        } else if (member_type == "way") {
          m.type = element_type_way;
        } else if (member_type == "relation") {
          m.type = element_type_relation;
        } else {
          throw std::runtime_error(
              (boost::format("Unknown member type `%1%'.") % member_type)
                  .str());
        }

        m.ref = get_attribute<osm_nwr_id_t>("ref", 4, attributes);
        m.role = get_attribute<std::string>("role", 5, attributes);

        parser->m_cur_rel->m_members.push_back(m);
      }
    }
  }

  static void warning(void *, const char *, ...) {
    // TODO
  }

  static void error(void *, const char *fmt, ...) {
    char buffer[1024];
    va_list arg_ptr;
    va_start(arg_ptr, fmt);
    vsnprintf(buffer, sizeof(buffer) - 1, fmt, arg_ptr);
    va_end(arg_ptr);
    throw std::runtime_error((boost::format("XML ERROR: %1%") % buffer).str());
  }

  database *m_db;
  node *m_cur_node;
  way *m_cur_way;
  relation *m_cur_rel;
  tags_t *m_cur_tags;
};

boost::shared_ptr<database> parse_xml(const char *filename) {
  xmlSAXHandler handler;
  memset(&handler, 0, sizeof(handler));

  handler.initialized = XML_SAX2_MAGIC;
  handler.startElement = &xml_parser::start_element;
  handler.warning = &xml_parser::warning;
  handler.error = &xml_parser::error;

  boost::shared_ptr<database> db = boost::make_shared<database>();
  xml_parser parser(db.get());
  int status = xmlSAXUserParseFile(&handler, &parser, filename);
  if (status != 0) {
    xmlErrorPtr err = xmlGetLastError();
    throw std::runtime_error(
        (boost::format("XML ERROR: %1%.") % err->message).str());
  }

  xmlCleanupParser();

  return db;
}

struct static_data_selection : public data_selection {
  static_data_selection(boost::shared_ptr<database> db) : m_db(db) {}
  virtual ~static_data_selection() {}

  virtual void write_nodes(output_formatter &formatter) {
    BOOST_FOREACH(osm_nwr_id_t id, m_nodes) {
      boost::optional<const node &> maybe_node = find_current<node>(id);
      if (maybe_node) {
        const node &n = *maybe_node;
        formatter.write_node(n.m_info, n.m_lon, n.m_lat, n.m_tags);
      }
    }
  }

  virtual void write_ways(output_formatter &formatter) {
    BOOST_FOREACH(osm_nwr_id_t id, m_ways) {
      boost::optional<const way &> maybe_way = find_current<way>(id);
      if (maybe_way) {
        const way &w = *maybe_way;
        formatter.write_way(w.m_info, w.m_nodes, w.m_tags);
      }
    }
  }

  virtual void write_relations(output_formatter &formatter) {
    BOOST_FOREACH(osm_nwr_id_t id, m_relations) {
      boost::optional<const relation &> maybe_relation = find_current<relation>(id);
      if (maybe_relation) {
        const relation &r = *maybe_relation;
        formatter.write_relation(r.m_info, r.m_members, r.m_tags);
      }
    }
  }

  virtual visibility_t check_node_visibility(osm_nwr_id_t id) {
    boost::optional<const node &> maybe_node = find_current<node>(id);
    if (maybe_node) {
      if (maybe_node->m_info.visible) {
        return data_selection::exists;
      } else {
        return data_selection::deleted;
      }
    } else {
      return data_selection::non_exist;
    }
  }

  virtual visibility_t check_way_visibility(osm_nwr_id_t id) {
    boost::optional<const way &> maybe_way = find_current<way>(id);
    if (maybe_way) {
      if (maybe_way->m_info.visible) {
        return data_selection::exists;
      } else {
        return data_selection::deleted;
      }
    } else {
      return data_selection::non_exist;
    }
  }

  virtual visibility_t check_relation_visibility(osm_nwr_id_t id) {
    boost::optional<const relation &> maybe_relation = find_current<relation>(id);
    if (maybe_relation) {
      if (maybe_relation->m_info.visible) {
        return data_selection::exists;
      } else {
        return data_selection::deleted;
      }
    } else {
      return data_selection::non_exist;
    }
  }

  virtual int select_nodes(const std::vector<osm_nwr_id_t> &ids) {
    int selected = 0;
    BOOST_FOREACH(osm_nwr_id_t id, ids) {
      boost::optional<const node &> n = find_current<node>(id);
      if (n) {
        m_nodes.insert(id);
        ++selected;
      }
    }
    return selected;
  }

  virtual int select_ways(const std::vector<osm_nwr_id_t> &ids) {
    int selected = 0;
    BOOST_FOREACH(osm_nwr_id_t id, ids) {
      boost::optional<const way &> w = find_current<way>(id);
      if (w) {
        m_ways.insert(id);
        ++selected;
      }
    }
    return selected;
  }

  virtual int select_relations(const std::vector<osm_nwr_id_t> &ids) {
    int selected = 0;
    BOOST_FOREACH(osm_nwr_id_t id, ids) {
      boost::optional<const relation &> r = find_current<relation>(id);
      if (r) {
        m_relations.insert(id);
        ++selected;
      }
    }
    return selected;
  }

  virtual int select_nodes_from_bbox(const bbox &bounds, int max_nodes) {
    typedef std::map<id_version, node> node_map_t;
    int selected = 0;
    const node_map_t::const_iterator end = m_db->m_nodes.end();
    for (node_map_t::const_iterator itr = m_db->m_nodes.begin();
         itr != end; ++itr) {
      node_map_t::const_iterator next = itr; ++next;
      const node &n = itr->second;
      if ((next == end || next->second.m_info.id != n.m_info.id) &&
          (n.m_lon >= bounds.minlon) && (n.m_lon <= bounds.maxlon) &&
          (n.m_lat >= bounds.minlat) && (n.m_lat <= bounds.maxlat) &&
          (n.m_info.visible)) {
        m_nodes.insert(n.m_info.id);
        ++selected;

        if (selected > max_nodes) {
          break;
        }
      }
    }
    return selected;
  }

  virtual void select_nodes_from_relations() {
    BOOST_FOREACH(osm_nwr_id_t id, m_relations) {
      boost::optional<const relation &> r = find_current<relation>(id);
      if (r) {
        BOOST_FOREACH(const member_info &m, r->m_members) {
          if (m.type == element_type_node) {
            m_nodes.insert(m.ref);
          }
        }
      }
    }
  }

  virtual void select_ways_from_nodes() {
    typedef std::map<id_version, way> way_map_t;
    const way_map_t::const_iterator end = m_db->m_ways.end();
    for (way_map_t::const_iterator itr = m_db->m_ways.begin();
         itr != end; ++itr) {
      way_map_t::const_iterator next = itr; ++next;
      const way &w = itr->second;
      if (next == end || next->second.m_info.id != w.m_info.id) {
        BOOST_FOREACH(osm_nwr_id_t node_id, w.m_nodes) {
          if (m_nodes.count(node_id) > 0) {
            m_ways.insert(w.m_info.id);
            break;
          }
        }
      }
    }
  }

  virtual void select_ways_from_relations() {
    BOOST_FOREACH(osm_nwr_id_t id, m_relations) {
      boost::optional<const relation &> r = find_current<relation>(id);
      if (r) {
        BOOST_FOREACH(const member_info &m, r->m_members) {
          if (m.type == element_type_way) {
            m_ways.insert(m.ref);
          }
        }
      }
    }
  }

  virtual void select_relations_from_ways() {
    typedef std::map<id_version, relation> relation_map_t;
    const relation_map_t::const_iterator end = m_db->m_relations.end();
    for (relation_map_t::const_iterator itr = m_db->m_relations.begin();
         itr != end; ++itr) {
      relation_map_t::const_iterator next = itr; ++next;
      const relation &r = itr->second;
      if (next == end || next->second.m_info.id != r.m_info.id) {
        BOOST_FOREACH(const member_info &m, r.m_members) {
          if ((m.type == element_type_way) && (m_ways.count(m.ref) > 0)) {
            m_relations.insert(r.m_info.id);
            break;
          }
        }
      }
    }
  }

  virtual void select_nodes_from_way_nodes() {
    BOOST_FOREACH(osm_nwr_id_t id, m_ways) {
      boost::optional<const way &> w = find_current<way>(id);
      if (w) {
        m_nodes.insert(w->m_nodes.begin(), w->m_nodes.end());
      }
    }
  }

  virtual void select_relations_from_nodes() {
    typedef std::map<id_version, relation> relation_map_t;
    const relation_map_t::const_iterator end = m_db->m_relations.end();
    for (relation_map_t::const_iterator itr = m_db->m_relations.begin();
         itr != end; ++itr) {
      relation_map_t::const_iterator next = itr; ++next;
      const relation &r = itr->second;
      BOOST_FOREACH(const member_info &m, r.m_members) {
        if ((m.type == element_type_node) && (m_nodes.count(m.ref) > 0)) {
          m_relations.insert(r.m_info.id);
          break;
        }
      }
    }
  }

  virtual void select_relations_from_relations() {
    std::set<osm_nwr_id_t> tmp_relations;
    typedef std::map<id_version, relation> relation_map_t;
    const relation_map_t::const_iterator end = m_db->m_relations.end();
    for (relation_map_t::const_iterator itr = m_db->m_relations.begin();
         itr != end; ++itr) {
      relation_map_t::const_iterator next = itr; ++next;
      const relation &r = itr->second;
      BOOST_FOREACH(const member_info &m, r.m_members) {
        if ((m.type == element_type_relation) &&
            (m_relations.count(m.ref) > 0)) {
          tmp_relations.insert(r.m_info.id);
          break;
        }
      }
    }
    m_relations.insert(tmp_relations.begin(), tmp_relations.end());
  }

  virtual void select_relations_members_of_relations() {
    BOOST_FOREACH(osm_nwr_id_t id, m_relations) {
      boost::optional<const relation &> r = find_current<relation>(id);
      if (r) {
        BOOST_FOREACH(const member_info &m, r->m_members) {
          if (m.type == element_type_relation) {
            m_relations.insert(m.ref);
          }
        }
      }
    }
  }

private:
  template <typename T>
  const std::map<id_version, T> &map_of() const;

  template <typename T>
  boost::optional<const T&> find_current(osm_nwr_id_t id) const {
    typedef std::map<id_version, T> element_map_t;
    id_version idv(id);
    const element_map_t &m = map_of<T>();
    if (!m.empty()) {
      typename element_map_t::const_iterator itr = m.upper_bound(idv);
      if (itr != m.begin()) {
        --itr;
        if (itr->first.id == id) {
          return itr->second;
        }
      }
    }
    return boost::none;
  }

  boost::shared_ptr<database> m_db;
  std::set<osm_nwr_id_t> m_nodes, m_ways, m_relations;
};

template <>
const std::map<id_version, node> &static_data_selection::map_of<node>() const {
  return m_db->m_nodes;
}

template <>
const std::map<id_version, way> &static_data_selection::map_of<way>() const {
  return m_db->m_ways;
}

template <>
const std::map<id_version, relation> &static_data_selection::map_of<relation>() const {
  return m_db->m_relations;
}

struct factory : public data_selection::factory {
  factory(const std::string &file) : m_database(parse_xml(file.c_str())) {}

  virtual ~factory() {}

  virtual boost::shared_ptr<data_selection> make_selection() {
    return boost::make_shared<static_data_selection>(m_database);
  }

private:
  boost::shared_ptr<database> m_database;
};

struct staticxml_backend : public backend {
  staticxml_backend()
      : m_name("staticxml"), m_options("Static XML backend options") {
    m_options.add_options()("file", po::value<string>(),
                            "file to load static OSM XML from.");
  }
  virtual ~staticxml_backend() {}

  const string &name() const { return m_name; }
  const po::options_description &options() const { return m_options; }

  shared_ptr<data_selection::factory> create(const po::variables_map &opts) {
    std::string file = opts["file"].as<std::string>();
    return boost::make_shared<factory>(file);
  }

  boost::shared_ptr<oauth::store> create_oauth_store(
    const po::variables_map &opts) {
    return boost::shared_ptr<oauth::store>();
  }

private:
  string m_name;
  po::options_description m_options;
};

} // anonymous namespace

boost::shared_ptr<backend> make_staticxml_backend() {
  return boost::make_shared<staticxml_backend>();
}
