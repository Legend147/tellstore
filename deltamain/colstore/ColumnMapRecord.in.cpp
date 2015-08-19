namespace tell {
namespace store {
namespace deltamain {
namespace impl {

#include "ColumnMapUtils.in.cpp" // includes convenience functions for colum-layout

/**
 * The memory layout of a column-map MV-DMRecord depends on the memory layout of a
 * column map page which is layed out the following way:
 *
 * - count: uint32 to store the number of records that are actually stored in
 *   this page. We also define:
 *   count^: count rounded up to the next multiple of 2. This helps with proper
 *   8-byte alignment of values.
 * - 4-byte padding
 * - key-version column: an array of size count of 16-byte values in format:
 *   |key (8 byte)|version (8 byte)|
 * - newest-pointers: an array of size count of 8-byte pointers to newest
 *   versions of records in the logs (as their is only one newest ptr per MV
 *   record and not per record version, only the first newestptr of every MV
 *   record is valid)
 * - null-bitmatrix: a bitmatrix of size capacity x (|Columns|+7)/8 bytes
 * - var-size-meta-data column: an array of size count^ of signed 4-byte values
 *   indicating the total size of all var-sized values of each record. This is
 *   used to allocate enough space for a record on a get request.
 *   MOREOVER: We set this size to zero to denote a version of a deleted tuple
 *   and to a negative number if the tuple is marked as reverted and will be
 *   deleted at the next GC phase. In that case, the absolute denotes the size.
 * - fixed-sized data columns: for each column there is an array of size
 *   count^ x value-size (4 or 8 bytes, as defined in schema)
 * - var-sized data columns: for each colum there is an array of
 *   count  x 8 bytes in format:
 *   |4-byte-offset from page start into var-sized heap|4-byte prefix of value|
 * - var-sized heap: values referred from var-sized columns in the format
 *   |4-byte size (including the size field)|value|
 *
 * Pointers into a page (e.g. from log) point to the first key/version entry in
 * the key-version column, but have the second lowest bit set to 1 (in order to
 * make clear it is a columnMap-MV record). This bit has to be unset (by subtracting
 * 2) in order to get the correct address.
 *
 * MV records are stored as single records in such a way that they are clustered
 * together and ordered by version DESC (which facilitates get-operations).
 */

//TODO: check whether this loop does the correct thing... doesn't
//this assume that the newestPtr is stored at the beginning of a
//log record (which is actually not the case)?
/**
 * The following macro is used to chare the common code in getNewest and casNewest.
 */
#define GET_NEWEST auto ptr = reinterpret_cast<std::atomic<uint64_t>*>(const_cast<char*>(getNewestPtrAt(index, basePtr, recordCount))); \
    auto p = ptr->load(); \
    while (ptr->load() % 2) { \
        ptr = reinterpret_cast<std::atomic<uint64_t>*>(p - 1); \
        p = ptr->load(); \
    }

template<class T>
class ColMapMVRecordBase {
protected:
    T mData;
public:
    using Type = typename DMRecordImplBase<T>::Type;
    ColMapMVRecordBase(T data) : mData(data) {}

    T getNewest(const Table *table, const uint32_t index, const char * basePtr, const uint32_t recordCount) const {
        LOG_ASSERT(table != nullptr, "table ptr must be set to a non-NULL value!");
        GET_NEWEST
        return reinterpret_cast<char*>(p);
    }

    T dataPtr() {
        LOG_ERROR("You are not supposed to call this on a ColMapMVRecord");
        std::terminate();
    }

    bool isValidDataRecord() const {
    //    //TODO: once you need this function, you have to add Table *table as an argument
    //    COMPUTE_BASE_KNOWLEDGE(mData, table)
    //    size_t nullBitMapSize = getNullBitMapSize(table);
    //    auto key = getKeyAt(index, basePtr);
    //    auto varLength = getVarsizedLenghtAt(index, basePtr, capacity, nullBitMapSize);
    //    for (; ; index++, key += 2, varLength++) {
    //        if (*varLength > 0) return true;
    //        if (key[2] != key[0])  // loop exit condition
    //            break;
    //    }
    //    return false;
        LOG_ERROR("You are not supposed to call this on a ColMapMVRecord");
        std::terminate();
    }

    void revert(uint64_t version) {
        LOG_ERROR("You are not supposed to call this on a ColMapMVRecord");
        std::terminate();
    }

    bool casNewest(const char* expected, const char* desired, const Table *table) const {
        COMPUTE_BASE_KNOWLEDGE(mData, table)
        GET_NEWEST
        uint64_t exp = reinterpret_cast<const uint64_t>(expected);
        uint64_t des = reinterpret_cast<const uint64_t>(desired);
        if (p != exp) return false;
        return ptr->compare_exchange_strong(exp, des);
    }

    int32_t getNumberOfVersions() const {
        LOG_ERROR("You are not supposed to call this on a ColMapMVRecord");
        std::terminate();
    }

    const uint64_t* versions() const {
        LOG_ERROR("You are not supposed to call this on a ColMapMVRecord");
        std::terminate();
    }

    const int32_t* offsets() const {
        LOG_ERROR("You are not supposed to call this on a ColMapMVRecord");
        std::terminate();
    }

    uint64_t size() const {
        LOG_ERROR("You are not supposed to call this on a ColMapMVRecord");
        std::terminate();
    }

    bool needsCleaning(uint64_t lowestActiveVersion, InsertMap& insertMap) const {
        LOG_ERROR("You are not supposed to call this on a ColMapMVRecord, call it directly on the page instead");
        std::terminate();
    }

    /**
     * BE CAREFUL: in contrast to the row-oriented variant, this call might actually
     * allocate new data (in sequential row-format) if the copyData flag is set (which
     * it is by default). In that case the ptr the newly allocated data is returned,
     * otherwise a nullptr is returned.
     */
    const char *data(const commitmanager::SnapshotDescriptor& snapshot,
                     size_t& size,
                     uint64_t& version,
                     bool& isNewest,
                     bool& isValid,
                     bool* wasDeleted,
                     const Table *table,
                     bool copyData
    ) const {
        COMPUTE_BASE_KNOWLEDGE(mData, table)
        const size_t nullBitMapSize = getNullBitMapSize(table);

        auto newest = getNewest(table, index, basePtr, recordCount);
        if (newest) {
            DMRecordImplBase<T> rec(newest);
            bool b;
            size_t s;
            auto res = rec.data(snapshot, s, version, isNewest, isValid, &b);
            if (isValid) {
                if (b || res) {
                    if (wasDeleted) *wasDeleted = b;
                    size = s;
                    return res;
                }
                isNewest = false;
            }
        }
        isValid = false;

        bool found = false;
        auto key = getKeyAt(index, basePtr);
        auto varLength = getVarsizedLenghtAt(index, basePtr, recordCount, nullBitMapSize);
        for (; ; index++, key += 2, varLength++) {
            if (*varLength < 0) continue;
            isValid = true;
            if (snapshot.inReadSet(key[1])) {   // key[0]: key, key[1]: version
                version = key[1];
                found = true;
                break;
            }
            isNewest = false;
            if (key[2] != key[0])  // loop exit condition
                break;
        }
        // index, varLength and key should have the right values

        if (!found) {
            if (wasDeleted) *wasDeleted = false;
            return nullptr;
        }

        if (*varLength != 0)
        {
            if (wasDeleted) *wasDeleted = false;
            if (copyData) {

                auto fixedSizeFields = table->getNumberOfFixedSizedFields();
                uint32_t recordSize = table->getFieldOffset(table->getNumberOfFixedSizedFields())
                        + *getVarsizedLenghtAt(index, basePtr, recordCount, getNullBitMapSize(table));
                char *res = reinterpret_cast<char*>(crossbow::allocator::malloc(recordSize));
                char *src;
                char *dest = res;
                // copy nullbitmap
                src = const_cast<char *>(getNullBitMapAt(index, basePtr, recordCount, nullBitMapSize));
                memcpy(dest, src, nullBitMapSize);
                dest += nullBitMapSize;

                // copy fixed-sized columns
                for (uint i = 0; i < fixedSizeFields; i++)
                {
                    src = const_cast<char*>(getColumnNAt(table, i, index, basePtr, recordCount, nullBitMapSize));
                    auto fieldSize = table->getFieldSize(i);
                    memcpy(dest, src, fieldSize);
                    dest += fieldSize;
                }
                // copy var-sized colums in a batch
                src = const_cast<char *>(getColumnNAt(table, fixedSizeFields, index, basePtr, recordCount, nullBitMapSize));
                src = const_cast<char *>(basePtr + *(reinterpret_cast<uint32_t *>(src)));   // pointer to first field in var-sized heap
                memcpy(dest, src, *varLength);

                // release buffer (which is ensure by the epoch-mechanism of crossbow-alloctor to not be garbage-collected too early)
                crossbow::allocator::free(res);
                return res;
            }
            return nullptr;
        }
        if (wasDeleted)
            *wasDeleted = true;
        return nullptr;
    }

    Type typeOfNewestVersion(bool& isValid) const {
    //    //TODO: once you need this function, you have to add Table *table as an argument
    //    COMPUTE_BASE_KNOWLEDGE(mData, table)
    //    size_t nullBitMapSize = getNullBitMapSize(table);
    //    auto newest = getNewest();
    //    if (newest) {
    //        DMRecordImplBase<T> rec(newest);
    //        auto res = rec.typeOfNewestVersion(isValid);
    //        if (isValid) return res;
    //    }
    //    isValid = true;
    //    auto key = getKeyAt(index, basePtr);
    //    auto varLength = getVarsizedLenghtAt(index, basePtr, capacity, nullBitMapSize);
    //    for (; ; index++, key += 2, varLength++) {
    //        if (*varLength > 0) return Type::MULTI_VERSION_RECORD;
    //        if (key[2] != key[0])  // loop exit condition
    //            break;
    //    }
    //    isValid = false;
    //    return Type::MULTI_VERSION_RECORD;
        LOG_ERROR("You are not supposed to call this on a ColMapMVRecord");
        std::terminate();
    }

    uint64_t copyAndCompact(
            uint64_t lowestActiveVersion,
            InsertMap& insertMap,
            char* dest,
            uint64_t maxSize,
            bool& success) const
    {
        LOG_ERROR("You are not supposed to call this on a ColMapMVRecord, call it directly on the page instead");
        std::terminate();
    }

    void collect(impl::VersionMap&, bool&, bool&) const {
        LOG_ASSERT(false, "should never call collect on MVRecord");
        std::cerr << "Fatal error!" << std::endl;
        std::terminate();
    }

};

template<class T>
struct ColMapMVRecord : ColMapMVRecordBase<T> {
    ColMapMVRecord(T data) : ColMapMVRecordBase<T>(data) {}
};

template<>
struct ColMapMVRecord<char*> : GeneralUpdates<ColMapMVRecordBase<char*>> {
    ColMapMVRecord(char* data) : GeneralUpdates<ColMapMVRecordBase<char*>>(data) {}
    void writeVersion(uint64_t) {
        LOG_ERROR("You are not supposed to call this on a MVRecord");
        std::terminate();
    }
    void writePrevious(const char*) {
        LOG_ERROR("You are not supposed to call this on a MVRecord");
        std::terminate();
    }
    void writeData(size_t, const char*) {
        LOG_ERROR("You are not supposed to call this on a MVRecord");
        std::terminate();
    }

    uint64_t *versions() {
        LOG_ERROR("You are not supposed to call this on MVRecord");
        std::terminate();
    }

    int32_t *offsets() {
        LOG_ERROR("You are not supposed to call this on MVRecord");
        std::terminate();
    }

    char *dataPtr() {
        LOG_ERROR("You are not supposed to call this on MVRecord");
        std::terminate();
    }

    bool update(char* next,
                bool& isValid,
                const commitmanager::SnapshotDescriptor& snapshot,
                const Table *table) {
        COMPUTE_BASE_KNOWLEDGE(mData, table)
        auto newest = getNewest(table, index, basePtr, recordCount);
        if (newest) {
            DMRecord rec(newest);
            bool res = rec.update(next, isValid, snapshot, table);
            if (!res && isValid) return false;
            if (isValid) {
                if (rec.type() == ColMapMVRecord::Type::MULTI_VERSION_RECORD) return res;
                return casNewest(newest, next, table);
            }
        }

        auto key = getKeyAt(index, basePtr);
        auto varLength = getVarsizedLenghtAt(index, basePtr, recordCount, getNullBitMapSize(table));
        for (; ; index++, key += 2, varLength++) {
            if (*varLength >= 0) break;
            if (key[2] != key[0])  // loop exit condition
                isValid = false;
                return false;
        }
        isValid = true;
        if (snapshot.inReadSet(*getVersionAt(index, basePtr)))
            return false;
        DMRecord nextRec(next);
        nextRec.writePrevious(this->mData);
        return casNewest(newest, next, table);
    }
};

} // namespace impl
} // namespace deltamain
} // namespace store
} // namespace tell
