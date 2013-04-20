#include "ScanCache.hpp"
#include "MemAllocHeap.hpp"
#include "MemAllocLinear.hpp"
#include "ScanData.hpp"
#include "Hash.hpp"
#include "BinaryWriter.hpp"
#include "Atomic.hpp"
#include "Stats.hpp"
#include "MemoryMappedFile.hpp"
#include "SortedArrayUtil.hpp"

#include <algorithm>
#include <time.h>

namespace t2
{

void ComputeScanCacheKey(
    HashDigest*        key_out,
    const char*        filename,
    const HashDigest&  scanner_hash)
{
  uint64_t path_hash = Djb2HashPath64(filename);
  key_out->m_Words.m_A = scanner_hash.m_Words.m_A ^ path_hash;
  key_out->m_Words.m_B = scanner_hash.m_Words.m_B;
  key_out->m_Words.m_C = scanner_hash.m_Words.m_C;
}
    
void ScanCacheInit(ScanCache* self, MemAllocHeap* heap, MemAllocLinear* allocator)
{
  self->m_FrozenData       = nullptr;
  self->m_Heap             = heap;
  self->m_Allocator        = allocator;
  self->m_RecordCount      = 0;
  self->m_TableSize        = 0;
  self->m_Table            = nullptr;
  self->m_FrozenAccess     = nullptr;

  ReadWriteLockInit(&self->m_Lock);
}

void ScanCacheDestroy(ScanCache* self)
{
  HeapFree(self->m_Heap, self->m_FrozenAccess);
  HeapFree(self->m_Heap, self->m_Table);
  ReadWriteLockDestroy(&self->m_Lock);
}

void ScanCacheSetCache(ScanCache* self, const ScanData* frozen_data)
{
  self->m_FrozenData = frozen_data;

  if (frozen_data)
  {
    self->m_FrozenAccess = HeapAllocateArrayZeroed<uint8_t>(self->m_Heap, frozen_data->m_EntryCount);

    Log(kDebug, "Scan cache initialized from frozen data - %u entries", frozen_data->m_EntryCount);

#if ENABLED(CHECKED_BUILD)
    // Paranoia - make sure the cache is sorted.
    for (int i = 1, count = frozen_data->m_EntryCount; i < count; ++i)
    {
      if (frozen_data->m_Keys[i] < frozen_data->m_Keys[i - 1])
        Croak("Header scanning cache is not sorted");
    }
    
#endif
  }
}

static ScanCache::Record* LookupDynamic(ScanCache* self, const HashDigest& key)
{
  uint32_t table_size = self->m_TableSize;

  if (table_size > 0)
  {
    uint32_t hash = key.m_Words.m_C;
    uint32_t index = hash & (table_size - 1);

    ScanCache::Record* chain = self->m_Table[index];
    while (chain)
    {
      if (key == chain->m_Key)
      {
        return chain;
      }

      chain = chain->m_Next;
    }
  }

  return nullptr;
}

bool ScanCacheLookup(ScanCache* self, const HashDigest& key, uint64_t timestamp, ScanCacheLookupResult* result_out, MemAllocLinear* scratch)
{
  bool success = false;

  result_out->m_IncludedFileCount = 0;
  result_out->m_IncludedFiles     = nullptr;

  ReadWriteLockRead(&self->m_Lock);

  if (ScanCache::Record* record = LookupDynamic(self, key))
  {
    if (record->m_FileTimestamp == timestamp)
    {
      result_out->m_IncludedFileCount = record->m_IncludeCount;
      result_out->m_IncludedFiles     = record->m_Includes;
      success                         = true;
    }
  }

  ReadWriteUnlockRead(&self->m_Lock);

  if (success)
  {
    AtomicIncrement(&g_Stats.m_NewScanCacheHits);
    return true;
  }

  const ScanData* scan_data = self->m_FrozenData;

  if (!scan_data)
  {
    AtomicIncrement(&g_Stats.m_ScanCacheMisses);
    return false;
  }

  uint32_t count = scan_data->m_EntryCount;

  // Check previously cached data. No lock needed for this as it is purely read-only
  const HashDigest* begin = scan_data->m_Keys;
  const HashDigest* end   = scan_data->m_Keys + count;
  const HashDigest* ptr = std::lower_bound(begin, end, key);

  if (ptr != end && *ptr == key)
  {
    int                   index      = int(ptr - begin);
    const ScanCacheEntry *entry      = scan_data->m_Data.Get() + index;

    if (entry->m_FileTimestamp == timestamp)
    {
      int                   file_count = entry->m_IncludedFiles.GetCount();

      const char** output = LinearAllocateArray<const char*>(scratch, file_count);

      for (int i = 0; i < file_count; ++i)
      {
        output[i] = entry->m_IncludedFiles[i];
      }

      result_out->m_IncludedFileCount = file_count;
      result_out->m_IncludedFiles     = output;
      success                         = true;

      // Flag this frozen record as having being accesses, so we don't throw it
      // away due to timing out. This is technically a race, but we trust CPUs
      // to sort out the cache line sharing via their cache coherency model.
      self->m_FrozenAccess[index]     = 1;
    }
  }

  if (success)
    AtomicIncrement(&g_Stats.m_OldScanCacheHits);
  else
    AtomicIncrement(&g_Stats.m_ScanCacheMisses);

  return success;
}

static void ScanCachePrepareInsert(ScanCache* self)
{
  // Check if a rehash is needed.
  size_t        old_size = self->m_TableSize;

  if (old_size > 0)
  {
    int64_t load = 0x100 * self->m_RecordCount / old_size;
    if (load < 0xc0)
      return;
  }

  MemAllocHeap *heap     = self->m_Heap;
  size_t        new_size = NextPowerOfTwo(uint32_t(old_size + 1));

  if (new_size < 64)
    new_size = 64;

  ScanCache::Record** old_table = self->m_Table;
  ScanCache::Record** new_table = HeapAllocateArrayZeroed<ScanCache::Record*>(heap, new_size);

  for (size_t i = 0; i < old_size; ++i)
  {
    ScanCache::Record* r = old_table[i];
    while (r)
    {
      ScanCache::Record *next  = r->m_Next;
      uint32_t           hash  = r->m_Key.m_Words.m_C;
      uint32_t           index = hash &(new_size - 1);

      r->m_Next        = new_table[index];
      new_table[index] = r;

      r                = next;
    }
  }

  self->m_TableSize = (uint32_t) new_size;
  self->m_Table     = new_table;

  HeapFree(heap, old_table);
}

void ScanCacheInsert(
    ScanCache*          self,
    const HashDigest&   key,
    uint64_t            timestamp,
    const char**        included_files,
    int                 count)
{
  AtomicIncrement(&g_Stats.m_ScanCacheInserts);

  ReadWriteLockWrite(&self->m_Lock);

  ScanCache::Record* record = LookupDynamic(self, key);

  // See if we have this record already (races to insert same include set are possible)
  if (nullptr == record || record->m_FileTimestamp != timestamp)
  {
    // Make sure we have room to insert.
    ScanCachePrepareInsert(self);

    uint32_t table_size = self->m_TableSize;
    uint32_t hash       = key.m_Words.m_C;
    uint32_t index      = hash &(table_size - 1);

    // Allocate a new record if needed
    const bool is_fresh = record == nullptr;

    if (is_fresh)
    {
      record        = LinearAllocate<ScanCache::Record>(self->m_Allocator);
      record->m_Key = key;
    }

    record->m_FileTimestamp = timestamp;
    record->m_IncludeCount  = count;
    record->m_Includes      = LinearAllocateArray<const char*>(self->m_Allocator, count);

    for (int i = 0; i < count; ++i)
    {
      record->m_Includes[i] = StrDup(self->m_Allocator, included_files[i]);
    }

    if (is_fresh)
    {
      record->m_Next       = self->m_Table[index];
      self->m_Table[index] = record;
      self->m_RecordCount++;
    }
  }

  ReadWriteUnlockWrite(&self->m_Lock);
}

bool ScanCacheDirty(ScanCache* self)
{
  bool result;

  ReadWriteLockRead(&self->m_Lock);

  result = self->m_RecordCount > 0;

  ReadWriteUnlockRead(&self->m_Lock);

  return result;
}

static bool SortRecordsByHash(const ScanCache::Record* l, const ScanCache::Record* r)
{
  return l->m_Key < r->m_Key;
}

struct ScanCacheWriter
{
  BinaryWriter   m_Writer;
  BinarySegment *m_MainSeg;
  BinarySegment *m_DigestSeg;
  BinarySegment *m_DataSeg;
  BinarySegment *m_TimestampSeg;
  BinarySegment *m_ArraySeg;
  BinarySegment *m_StringSeg;
  BinaryLocator  m_DigestPtr;
  BinaryLocator  m_EntryPtr;
  BinaryLocator  m_TimestampPtr;
  uint32_t       m_RecordsOut;

};

static void ScanCacheWriterInit(ScanCacheWriter* self, MemAllocHeap* heap)
{
  BinaryWriterInit(&self->m_Writer, heap);

  self->m_MainSeg      = BinaryWriterAddSegment(&self->m_Writer);
  self->m_DigestSeg    = BinaryWriterAddSegment(&self->m_Writer);
  self->m_DataSeg      = BinaryWriterAddSegment(&self->m_Writer);
  self->m_TimestampSeg = BinaryWriterAddSegment(&self->m_Writer);
  self->m_ArraySeg     = BinaryWriterAddSegment(&self->m_Writer);
  self->m_StringSeg    = BinaryWriterAddSegment(&self->m_Writer);

  self->m_DigestPtr    = BinarySegmentPosition(self->m_DigestSeg);
  self->m_EntryPtr     = BinarySegmentPosition(self->m_DataSeg);
  self->m_TimestampPtr = BinarySegmentPosition(self->m_TimestampSeg);

  self->m_RecordsOut   = 0;
}

static void ScanCacheWriterDestroy(ScanCacheWriter* self)
{
  BinaryWriterDestroy(&self->m_Writer);
}

static bool ScanCacheWriterFlush(ScanCacheWriter* self, const char* filename)
{
  BinarySegmentWriteUint32(self->m_MainSeg, ScanData::MagicNumber);
  BinarySegmentWriteUint32(self->m_MainSeg, self->m_RecordsOut);
  BinarySegmentWritePointer(self->m_MainSeg, self->m_DigestPtr);
  BinarySegmentWritePointer(self->m_MainSeg, self->m_EntryPtr);
  BinarySegmentWritePointer(self->m_MainSeg, self->m_TimestampPtr);

  return BinaryWriterFlush(&self->m_Writer, filename);
}

// Save frozen record unless timestamp is too old
template <typename T>
static void SaveRecord(
    ScanCacheWriter*    self,
    const HashDigest*   digest,
    T*                  includes,
    int                 include_count,
    uint64_t            file_timestamp,
    uint64_t            access_time)
{
  // TODO: Check access time to see if we should discard.
  BinarySegment *digest_seg    = self->m_DigestSeg;
  BinarySegment *data_seg      = self->m_DataSeg;
  BinarySegment *timestamp_seg = self->m_TimestampSeg;
  BinarySegment *string_seg    = self->m_StringSeg;
  BinarySegment *array_seg     = self->m_ArraySeg;

  BinaryLocator string_ptrs = BinarySegmentPosition(array_seg);

  for (int i = 0; i < include_count; ++i)
  {
    BinarySegmentWritePointer(array_seg, BinarySegmentPosition(string_seg));
    BinarySegmentWriteStringData(string_seg, includes[i]);
  }

  BinarySegmentWrite(digest_seg, (const char*) digest->m_Data, sizeof(HashDigest));

  BinarySegmentWriteUint64(data_seg, file_timestamp);
  BinarySegmentWriteUint32(data_seg, uint32_t(include_count));
  BinarySegmentWritePointer(data_seg, string_ptrs);

  BinarySegmentWriteUint64(timestamp_seg, access_time);

  self->m_RecordsOut++;
}

bool ScanCacheSave(ScanCache* self, const char* fn, MemoryMappedFile* prev_mapping, MemAllocHeap* heap)
{
  TimingScope timing_scope(nullptr, &g_Stats.m_ScanCacheSaveTime);

  MemAllocLinear* scratch = self->m_Allocator;

  MemAllocLinearScope scratch_scope(scratch);

  ScanCacheWriter writer;
  ScanCacheWriterInit(&writer, heap);

  // Save new view of the scan cache
  //
  // Algorithm:
  // 
  // - Get all records from the dynamic table (stuff we put in this session)
  const uint32_t      record_count = self->m_RecordCount;
  ScanCache::Record **dyn_records  = LinearAllocateArray<ScanCache::Record*>(scratch, record_count);

  {
    uint32_t records_out = 0;
    for (uint32_t ti = 0, tsize = self->m_TableSize; ti < tsize; ++ti)
    {
      ScanCache::Record* chain = self->m_Table[ti];
      while (chain)
      {
        dyn_records[records_out++] = chain;
        chain                      = chain->m_Next;
      }
    }

    CHECK(records_out == record_count);
  }

  // - Sort these records in key order (by SHA-1 hash)
  std::sort(dyn_records, dyn_records + record_count, SortRecordsByHash);

  const ScanData       *scan_data      = self->m_FrozenData;
  uint32_t              frozen_count   = scan_data ? scan_data->m_EntryCount : 0;
  const HashDigest     *frozen_digests = scan_data ? scan_data->m_Keys.Get() : nullptr;
  const ScanCacheEntry *frozen_entries = scan_data ? scan_data->m_Data.Get() : nullptr;
  const uint64_t       *frozen_times   = scan_data ? scan_data->m_AccessTimes.Get() : nullptr;
  const uint8_t        *frozen_access  = self->m_FrozenAccess;

  const uint64_t now = time(nullptr);

  // Keep old entries for a week.
  const uint64_t timestamp_cutoff = now - 60 * 60 * 24 * 7;

  auto key_dynamic = [=](size_t index) -> const HashDigest* { return &dyn_records[index]->m_Key; };
  auto key_frozen = [=](size_t index) { return frozen_digests + index; };

  auto save_dynamic = [&writer, dyn_records, now](size_t index)
  {
    SaveRecord(
        &writer,
        &dyn_records[index]->m_Key,
        dyn_records[index]->m_Includes,
        dyn_records[index]->m_IncludeCount,
        dyn_records[index]->m_FileTimestamp,
        now);
  };

  auto save_frozen = [&](size_t index)
  {
    uint64_t timestamp = frozen_times[index];
    if (frozen_access[index])
      timestamp = now;

    if (timestamp > timestamp_cutoff)
    {
      SaveRecord(
          &writer,
          frozen_digests + index, 
          frozen_entries[index].m_IncludedFiles.GetArray(),
          frozen_entries[index].m_IncludedFiles.GetCount(),
          frozen_entries[index].m_FileTimestamp,
          timestamp);
    }
  };

  TraverseSortedArrays(record_count, save_dynamic, key_dynamic, frozen_count, save_frozen, key_frozen);

  // Unmap the previous file from RAM so we can overwrite it on Windows.
  MmapFileUnmap(prev_mapping);

  self->m_FrozenData = nullptr;

  bool result = ScanCacheWriterFlush(&writer, fn);

  ScanCacheWriterDestroy(&writer);

  return result;
}

}