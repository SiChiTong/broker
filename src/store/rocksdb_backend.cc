#include <rocksdb/env.h>

#include "broker/broker.h"

#include "rocksdb_backend_impl.hh"
#include "../persistables.hh"
#include "../util/misc.hh"

namespace broker {
namespace store {

namespace detail {

template <class T>
static void to_serial(const T& obj, std::string& rval) {
  util::persist::save_archive saver(std::move(rval));
  save(saver, obj);
  rval = saver.get();
}

template <class T>
static std::string to_serial(const T& obj) {
  std::string rval;
  to_serial(obj, rval);
  return rval;
}

template <class T>
static std::string to_serial(const T& obj, char keyspace) {
  std::string rval{keyspace};
  to_serial(obj, rval);
  return rval;
}

template <class T>
static T from_serial(const char* blob, size_t num_bytes) {
  T rval;
  util::persist::load_archive loader(blob, num_bytes);
  load(loader, &rval);
  return rval;
}

template <class T, class C>
static T from_serial(const C& bytes) {
  return from_serial<T>(bytes.data(), bytes.size());
}

static rocksdb::Status
insert(rocksdb::DB* db, const data& k, const data& v,
       bool delete_expiry_if_nil,
       const maybe<expiration_time>& e = {}) {
  auto kserial = to_serial(k, 'a');
  auto vserial = to_serial(v);
  rocksdb::WriteBatch batch;
  batch.Put(kserial, vserial);
  kserial[0] = 'e';
  if (e) {
    auto evserial = to_serial(*e);
    batch.Put(kserial, evserial);
  } else if (delete_expiry_if_nil)
    batch.Delete(kserial);
  return db->Write({}, &batch);
}

} // namespace detail

rocksdb_backend::rocksdb_backend(uint64_t exact_size_threshold)
  : pimpl(new impl(exact_size_threshold)) {
}

rocksdb_backend::~rocksdb_backend() = default;

rocksdb_backend::rocksdb_backend(rocksdb_backend&&) = default;

rocksdb_backend& rocksdb_backend::operator=(rocksdb_backend&&) = default;

rocksdb::Status rocksdb_backend::open(std::string db_path,
                                      rocksdb::Options options) {
  rocksdb::DB* db;
  auto rval = rocksdb::DB::Open(options, db_path, &db);
  pimpl->db.reset(db);
  options.create_if_missing = true;
  pimpl->options = options;
  if (pimpl->require_ok(rval)) {
    // Use key-space prefix 'm' to store metadata, 'a' for application
    // data, and 'e' for expiration values.
    rval = pimpl->db->Put({}, "mbroker_version", BROKER_VERSION);
    pimpl->require_ok(rval);
    return rval;
  }
  return rval;
}

void rocksdb_backend::do_increase_sequence() {
  ++pimpl->sn;
}

std::string rocksdb_backend::do_last_error() const {
  return pimpl->last_error;
}

bool rocksdb_backend::do_init(snapshot sss) {
  if (!do_clear())
    return false;
  rocksdb::WriteBatch batch;
  for (const auto& kv : sss.entries) {
    auto kserial = detail::to_serial(kv.first, 'a');
    auto vserial = detail::to_serial(kv.second.item);
    batch.Put(kserial, vserial);
    if (kv.second.expiry) {
      kserial[0] = 'e';
      auto evserial = detail::to_serial(*kv.second.expiry);
      batch.Put(kserial, evserial);
    }
  }
  pimpl->sn = std::move(sss.sn);
  return pimpl->require_ok(pimpl->db->Write({}, &batch));
}

const sequence_num& rocksdb_backend::do_sequence() const {
  return pimpl->sn;
}

bool rocksdb_backend::do_insert(
  data k, data v, maybe<expiration_time> e) {
  if (!pimpl->require_db())
    return false;
  return pimpl->require_ok(detail::insert(pimpl->db.get(), k, v, true, e));
}

modification_result rocksdb_backend::do_increment(const data& k, int64_t by,
                                                  double mod_time) {
  auto op = do_lookup_expiry(k);
  if (!op)
    return {modification_result::status::failure, {}};
  if (!util::increment_data(op->first, by, &pimpl->last_error))
    return {modification_result::status::invalid, {}};
  auto new_expiry = util::update_last_modification(op->second, mod_time);
  if (pimpl->require_ok(
        detail::insert(pimpl->db.get(), k, *op->first, false, new_expiry)))
    return {modification_result::status::success, std::move(new_expiry)};
  return {modification_result::status::failure, {}};
}

modification_result rocksdb_backend::do_add_to_set(const data& k, data element,
                                                   double mod_time) {
  auto op = do_lookup_expiry(k);
  if (!op)
    return {modification_result::status::failure, {}};
  if (!util::add_data_to_set(op->first, std::move(element), &pimpl->last_error))
    return {modification_result::status::invalid, {}};
  auto new_expiry = util::update_last_modification(op->second, mod_time);
  if (pimpl->require_ok(
        detail::insert(pimpl->db.get(), k, *op->first, false, new_expiry)))
    return {modification_result::status::success, std::move(new_expiry)};
  return {modification_result::status::failure, {}};
}

modification_result rocksdb_backend::do_remove_from_set(const data& k, 
                                                        const data& element,
                                                        double mod_time) {
  auto op = do_lookup_expiry(k);
  if (!op)
    return {modification_result::status::failure, {}};
  if (!util::remove_data_from_set(op->first, element, &pimpl->last_error))
    return {modification_result::status::invalid, {}};
  auto new_expiry = util::update_last_modification(op->second, mod_time);
  if (pimpl->require_ok(
        detail::insert(pimpl->db.get(), k, *op->first, false, new_expiry)))
    return {modification_result::status::success, std::move(new_expiry)};
  return {modification_result::status::failure, {}};
}

bool rocksdb_backend::do_erase(const data& k) {
  if (!pimpl->require_db())
    return false;
  auto kserial = detail::to_serial(k, 'a');
  if (!pimpl->require_ok(pimpl->db->Delete({}, kserial)))
    return false;
  kserial[0] = 'e';
  return pimpl->require_ok(pimpl->db->Delete({}, kserial));
}

bool rocksdb_backend::do_erase(std::string kserial) {
  if (!pimpl->require_db())
    return false;
  kserial[0] = 'a';
  if (!pimpl->require_ok(pimpl->db->Delete({}, kserial)))
    return false;
  kserial[0] = 'e';
  return pimpl->require_ok(pimpl->db->Delete({}, kserial));
}

bool rocksdb_backend::do_expire(const data& k,
                                const expiration_time& expiration) {
  if (!pimpl->require_db())
    return false;
  auto kserial = detail::to_serial(k, 'e');
  std::string vserial;
  bool value_found;
  if (!pimpl->db->KeyMayExist({}, kserial, &vserial, &value_found))
    return true;
  if (value_found) {
    auto stored_expiration = detail::from_serial<expiration_time>(vserial);
    if (stored_expiration == expiration)
      return do_erase(std::move(kserial));
    else
      return true;
  }
  auto stat = pimpl->db->Get(rocksdb::ReadOptions{}, kserial, &vserial);
  if (stat.IsNotFound())
    return true;
  if (!pimpl->require_ok(stat))
    return false;
  auto stored_expiration = detail::from_serial<expiration_time>(vserial);
  if (stored_expiration == expiration)
    return do_erase(std::move(kserial));
  else
    return true;
}

bool rocksdb_backend::do_clear() {
  if (!pimpl->require_db())
    return false;
  std::string db_path = pimpl->db->GetName();
  pimpl->db.reset();
  auto stat = rocksdb::DestroyDB(db_path, rocksdb::Options{});
  if (!pimpl->require_ok(stat))
    return false;
  return pimpl->require_ok(open(std::move(db_path), pimpl->options));
}

modification_result rocksdb_backend::do_push_left(const data& k, vector items,
                                                  double mod_time) {
  auto op = do_lookup_expiry(k);
  if (!op)
    return {modification_result::status::failure, {}};
  if (!util::push_left(op->first, std::move(items), &pimpl->last_error))
    return {modification_result::status::invalid, {}};
  auto new_expiry = util::update_last_modification(op->second, mod_time);
  if (pimpl->require_ok(
        detail::insert(pimpl->db.get(), k, *op->first, false, new_expiry)))
    return {modification_result::status::success, std::move(new_expiry)};
  return {modification_result::status::failure, {}};
}

modification_result rocksdb_backend::do_push_right(const data& k, vector items,
                                                   double mod_time) {
  auto op = do_lookup_expiry(k);
  if (!op)
    return {modification_result::status::failure, {}};
  if (!util::push_right(op->first, std::move(items), &pimpl->last_error))
    return {modification_result::status::invalid, {}};
  auto new_expiry = util::update_last_modification(op->second, mod_time);
  if (pimpl->require_ok(
        detail::insert(pimpl->db.get(), k, *op->first, false, new_expiry)))
    return {modification_result::status::success, std::move(new_expiry)};
  return {modification_result::status::failure, {}};
}

std::pair<modification_result, maybe<data>>
rocksdb_backend::do_pop_left(const data& k, double mod_time) {
  auto op = do_lookup_expiry(k);
  if (!op)
    return {{modification_result::status::failure, {}}, {}};
  if (!op->first)
    // Fine, key didn't exist.
    return {{modification_result::status::success, {}}, {}};
  auto& v = *op->first;
  auto rval = util::pop_left(v, &pimpl->last_error);
  if (!rval)
    return {{modification_result::status::invalid, {}}, {}};
  if (!*rval)
    // Fine, popped an empty list.
    return {{modification_result::status::success, {}}, {}};
  auto new_expiry = util::update_last_modification(op->second, mod_time);
  if (pimpl->require_ok(detail::insert(pimpl->db.get(), k, v, false,
                                      new_expiry)))
    return {{modification_result::status::success, std::move(new_expiry)},
            std::move(*rval)};
  return {{modification_result::status::failure, {}}, {}};
}

std::pair<modification_result, maybe<data>>
rocksdb_backend::do_pop_right(const data& k, double mod_time) {
  auto op = do_lookup_expiry(k);
  if (!op)
    return {{modification_result::status::failure, {}}, {}};
  if (!op->first)
    // Fine, key didn't exist.
    return {{modification_result::status::success, {}}, {}};
  auto& v = *op->first;
  auto rval = util::pop_right(v, &pimpl->last_error);
  if (!rval)
    return {{modification_result::status::invalid, {}}, {}};
  if (!*rval)
    // Fine, popped an empty list.
    return {{modification_result::status::success, {}}, {}};
  auto new_expiry = util::update_last_modification(op->second, mod_time);
  if (pimpl->require_ok(detail::insert(pimpl->db.get(), k, v, false,
                                      new_expiry)))
    return {{modification_result::status::success, std::move(new_expiry)},
            std::move(*rval)};
  return {{modification_result::status::failure, {}}, {}};
}

maybe<maybe<data>>
rocksdb_backend::do_lookup(const data& k) const {
  if (!pimpl->require_db())
    return {};
  auto kserial = detail::to_serial(k, 'a');
  std::string vserial;
  bool value_found;
  if (!pimpl->db->KeyMayExist({}, kserial, &vserial, &value_found))
    return maybe<data>{};
  if (value_found)
    return {detail::from_serial<data>(vserial)};
  auto stat = pimpl->db->Get(rocksdb::ReadOptions{}, kserial, &vserial);
  if (stat.IsNotFound())
    return maybe<data>{};
  if (!pimpl->require_ok(stat))
    return {};
  return {detail::from_serial<data>(vserial)};
}

maybe<std::pair<maybe<data>, maybe<expiration_time>>>
rocksdb_backend::do_lookup_expiry(const data& k) const {
  if (!pimpl->require_db())
    return {};
  auto kserial = detail::to_serial(k, 'a');
  std::string vserial;
  bool value_found;
  if (!pimpl->db->KeyMayExist({}, kserial, &vserial, &value_found))
    return {std::make_pair(maybe<data>{},
                           maybe<expiration_time>{})};
  data value;
  if (value_found)
    value = detail::from_serial<data>(vserial);
  else {
    auto stat = pimpl->db->Get(rocksdb::ReadOptions{}, kserial, &vserial);
    if (stat.IsNotFound())
      return {std::make_pair(maybe<data>{},
                             maybe<expiration_time>{})};
    if (!pimpl->require_ok(stat))
      return {};
    value = detail::from_serial<data>(vserial);
  }
  kserial[0] = 'e';
  value_found = false;
  if (!pimpl->db->KeyMayExist({}, kserial, &vserial, &value_found))
    return {
      std::make_pair(std::move(value), maybe<expiration_time>{})};
  expiration_time expiry;
  if (value_found)
    expiry = detail::from_serial<expiration_time>(vserial);
  else {
    auto stat = pimpl->db->Get(rocksdb::ReadOptions{}, kserial, &vserial);
    if (stat.IsNotFound())
      return {
        std::make_pair(std::move(value), maybe<expiration_time>{})};
    if (!pimpl->require_ok(stat))
      return {};
    expiry = detail::from_serial<expiration_time>(vserial);
  }
  return {std::make_pair(std::move(value), std::move(expiry))};
}

maybe<bool> rocksdb_backend::do_exists(const data& k) const {
  if (!pimpl->require_db())
    return {};
  auto kserial = detail::to_serial(k, 'a');
  std::string vserial;
  if (!pimpl->db->KeyMayExist(rocksdb::ReadOptions{}, kserial, &vserial))
    return false;
  auto stat = pimpl->db->Get(rocksdb::ReadOptions{}, kserial, &vserial);
  if (stat.IsNotFound())
    return false;
  if (!pimpl->require_ok(stat))
    return {};
  return true;
}

maybe<std::vector<data>> rocksdb_backend::do_keys() const {
  if (!pimpl->require_db())
    return {};
  rocksdb::ReadOptions options;
  options.fill_cache = false;
  std::unique_ptr<rocksdb::Iterator> it(pimpl->db->NewIterator(options));
  std::vector<data> rval;
  for (it->Seek("a"); it->Valid() && it->key()[0] == 'a'; it->Next()) {
    auto s = it->key();
    s.remove_prefix(1);
    rval.emplace_back(detail::from_serial<data>(s));
  }
  if (!pimpl->require_ok(it->status()))
    return {};
  return rval;
}

maybe<uint64_t> rocksdb_backend::do_size() const {
  if (!pimpl->require_db())
    return {};
  uint64_t rval;
  if (pimpl->db->GetIntProperty("rocksdb.estimate-num-keys", &rval)
      && rval > pimpl->exact_size_threshold)
    return rval;
  rocksdb::ReadOptions options;
  options.fill_cache = false;
  std::unique_ptr<rocksdb::Iterator> it(pimpl->db->NewIterator(options));
  rval = 0;
  for (it->Seek("a"); it->Valid() && it->key()[0] == 'a'; it->Next())
    ++rval;
  if (pimpl->require_ok(it->status()))
    return rval;
  return {};
}

maybe<snapshot> rocksdb_backend::do_snap() const {
  if (!pimpl->require_db())
    return {};
  rocksdb::ReadOptions options;
  options.fill_cache = false;
  std::unique_ptr<rocksdb::Iterator> it(pimpl->db->NewIterator(options));
  snapshot rval;
  rval.sn = pimpl->sn;
  std::unordered_map<data, expiration_time> expiries;
  for (it->Seek("e"); it->Valid() && it->key()[0] == 'e'; it->Next()) {
    auto ks = it->key();
    auto vs = it->value();
    ks.remove_prefix(1);
    auto key = detail::from_serial<data>(ks);
    expiries[std::move(key)] = detail::from_serial<expiration_time>(vs);
  }
  if (!pimpl->require_ok(it->status()))
    return {};
  for (it->Seek("a"); it->Valid() && it->key()[0] == 'a'; it->Next()) {
    auto ks = it->key();
    auto vs = it->value();
    ks.remove_prefix(1);
    auto entry
      = std::make_pair(detail::from_serial<data>(ks),
                       value{detail::from_serial<data>(vs)});
    auto eit = expiries.find(entry.first);
    if (eit != expiries.end())
      entry.second.expiry = std::move(eit->second);
    rval.entries.emplace_back(std::move(entry));
  }
  if (!pimpl->require_ok(it->status()))
    return {};
  return rval;
}

maybe<std::deque<expirable>> rocksdb_backend::do_expiries() const {
  if (!pimpl->require_db())
    return {};
  rocksdb::ReadOptions options;
  options.fill_cache = false;
  std::unique_ptr<rocksdb::Iterator> it(pimpl->db->NewIterator(options));
  std::deque<expirable> rval;
  for (it->Seek("e"); it->Valid() && it->key()[0] == 'e'; it->Next()) {
    auto ks = it->key();
    auto vs = it->value();
    ks.remove_prefix(1);
    auto key = detail::from_serial<data>(ks);
    auto expiry = detail::from_serial<expiration_time>(vs);
    rval.emplace_back(expirable{std::move(key), std::move(expiry)});
  }
  if (!pimpl->require_ok(it->status()))
    return {};
  return rval;
}

} // namespace store
} // namespace broker

// Begin C API
#include "broker/broker.h"
using std::nothrow;

broker_store_rocksdb_backend* broker_store_rocksdb_backend_create() {
  auto rval = new (nothrow) broker::store::rocksdb_backend();
  return reinterpret_cast<broker_store_rocksdb_backend*>(rval);
}

void broker_store_rocksdb_backend_delete(broker_store_rocksdb_backend* b) {
  delete reinterpret_cast<broker::store::rocksdb_backend*>(b);
}

int broker_store_rocksdb_backend_open(broker_store_rocksdb_backend* b,
                                      const char* path, int create_if_missing) {
  auto bb = reinterpret_cast<broker::store::rocksdb_backend*>(b);
  rocksdb::Options options = {};
  options.create_if_missing = create_if_missing;
  return bb->open(path, options).ok();
}

const char*
broker_store_rocksdb_backend_last_error(const broker_store_rocksdb_backend* b) {
  auto bb = reinterpret_cast<const broker::store::rocksdb_backend*>(b);
  return bb->last_error().data();
}
