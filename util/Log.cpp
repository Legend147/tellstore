#include "Log.hpp"

#include "Epoch.hpp"

namespace tell {
namespace store {

static_assert(ATOMIC_POINTER_LOCK_FREE, "Atomic pointer operations not supported");
static_assert(sizeof(LogPage*) == sizeof(std::atomic<LogPage*>), "Atomics won't work correctly");
static_assert(sizeof(LogPage) <= LogPage::LOG_HEADER_SIZE, "LOG_HEADER_SIZE must be larger or equal than LogPage");
static_assert(sizeof(LogEntry) <= LogEntry::LOG_ENTRY_SIZE, "LOG_ENTRY_SIZE must be larger or equal than LogEntry");

uint32_t LogEntry::tryAcquire(uint32_t size, uint32_t type) {
    LOG_ASSERT(size != 0x0u, "Size has to be greater than zero");
    LOG_ASSERT((size >> 31) == 0, "MSB has to be zero");

    auto s = ((size << 1) | 0x1u);
    uint32_t exp = 0x0u;
    if (!mSize.compare_exchange_strong(exp, s)) {
        return entrySizeFromSize(exp >> 1);
    }
    mType = type;
    return 0x0u;
}

LogEntry* LogPage::append(uint32_t size, uint32_t type /* = 0x0u */) {
    auto entrySize = LogEntry::entrySizeFromSize(size);
    if (entrySize > LogPage::MAX_ENTRY_SIZE) {
        LOG_ASSERT(false, "Tried to append %d bytes but %d bytes is max", entrySize, LogPage::MAX_ENTRY_SIZE);
        return nullptr;
    }
    return appendEntry(size, entrySize, type);
}

LogEntry* LogPage::appendEntry(uint32_t size, uint32_t entrySize, uint32_t type) {
    auto offset = mOffset.load();

    // Check if page is already sealed
    if ((offset & 0x1u) == 0) {
        return nullptr;
    }
    auto position = (offset >> 1);

    while (true) {
        auto endPosition = position + entrySize;

        // Check if we have enough space in the log page
        if (endPosition > LogPage::MAX_ENTRY_SIZE) {
            return nullptr;
        }

        // Try to acquire the space for the new entry
        auto entry = reinterpret_cast<LogEntry*>(this->data() + position);
        LOG_ASSERT((reinterpret_cast<uintptr_t>(entry) % 16) == 8 , "Position is not 16 byte aligned with offset 8");

        auto res = entry->tryAcquire(size, type);
        if (res != 0) {
            position += res;
            continue;
        }

        // Try to set the new offset until we succeed or another thread set a higher offset
        auto nOffset = ((endPosition << 1) | 0x1u);
        while (offset < nOffset) {
            // Set new offset, if this fails offset will contain the new offset value
            if (mOffset.compare_exchange_strong(offset, nOffset)) {
                break;
            }
            // Check if page was sealed in the meantime
            if ((offset & 0x1u) == 0) {
                // Check if page was sealed after we completely acquired the space for the log entry
                if ((offset >> 1) >= endPosition) {
                    break;
                }

                // Page was sealed before we completely acquired the space for the log entry
                return nullptr;
            }
        }

        return entry;
    }
}

void BaseLogImpl::freeEmptyPageNow(LogPage* page) {
    memset(page, 0, LogPage::LOG_HEADER_SIZE);
    mPageManager.free(page);
}

void BaseLogImpl::freePage(LogPage* begin, LogPage* end) {
    auto& pageManager = mPageManager;
    allocator::invoke([begin, end, &pageManager]() {
        auto page = begin;
        while (page != end) {
            auto next = page->next().load();
            pageManager.free(page);
            page = next;
        }
    });
}

UnorderedLogImpl::UnorderedLogImpl(PageManager& pageManager)
        : BaseLogImpl(pageManager),
          mHead(LogHead(acquirePage(), nullptr)),
          mTail(mHead.load().writeHead),
          mPages(1) {
    LOG_ASSERT((reinterpret_cast<uintptr_t>(&mHead) % 16) == 0, "Head is not 16 byte aligned");
    LOG_ASSERT(mHead.is_lock_free(), "LogHead is not lock free");
    LOG_ASSERT(mHead.load().writeHead == mTail.load(), "Head and Tail do not point to the same page");
}

void UnorderedLogImpl::appendPage(LogPage* begin, LogPage* end) {
    auto oldHead = mHead.load();

    auto pages = 1;
    for (auto page = begin; page != end; page = page->next().load()) {
        ++pages;
    }
    mPages.fetch_add(pages);

    while (true) {
        // Next should point to the last appendHead or the writeHead if there are no pages waiting to be appended
        auto next = (oldHead.appendHead ? oldHead.appendHead : oldHead.writeHead);
        end->next().store(next);

        // Seal the old append head
        if (oldHead.appendHead) {
            oldHead.appendHead->seal();
        }

        // Try to update the head
        LogHead nHead(oldHead.writeHead, begin);
        if (mHead.compare_exchange_strong(oldHead, nHead)) {
            break;
        }
    }
}

void UnorderedLogImpl::erase(LogPage* begin, LogPage* end) {
    LOG_ASSERT(begin != nullptr, "Begin page must not be null");

    if (begin == end) {
        return;
    }

    if (!end) {
        mTail.store(begin);
    }

    auto next = begin->next().exchange(end);
    if (next == end) return;

    auto pages = 0;
    for (auto page = next; page != end; page = page->next().load()) {
        ++pages;
    }
    mPages.fetch_sub(pages);

    freePage(next, end);
}

LogEntry* UnorderedLogImpl::appendEntry(uint32_t size, uint32_t entrySize, uint32_t type) {
    auto head = mHead.load();
    while (head.writeHead) {
        // Try to append a new log entry to the page
        auto entry = head.writeHead->appendEntry(size, entrySize, type);
        if (entry != nullptr) {
            return entry;
        }

        // The page must be full, acquire a new one
        head = createPage(head);
    }

    // This can only happen if the page manager ran out of space
    return nullptr;
}

UnorderedLogImpl::LogHead UnorderedLogImpl::createPage(LogHead oldHead) {
    auto writeHead = oldHead.writeHead;

    // Seal the old write head so no one can append
    writeHead->seal();

    while (true) {
        bool freeHead = false;
        LogHead nHead(oldHead.appendHead, nullptr);

        // If the append head is null we have to allocate a new head page
        if (!oldHead.appendHead) {
            nHead.writeHead = acquirePage();
            if (!nHead.writeHead) {
                LOG_ERROR("PageManager ran out of space");
                return nHead;
            }
            ++mPages;
            nHead.writeHead->next().store(oldHead.writeHead);
            freeHead = true;
        }

        // Try to set the page as new head
        // If this fails then another thread already set a new page and oldHead will point to it
        if (!mHead.compare_exchange_strong(oldHead, nHead)) {
            // We either have a new write or append head so we can free the previously allocated page
            if (freeHead) {
                --mPages;
                freeEmptyPageNow(nHead.writeHead);
            }

            // Check if the write head is still the same (i.e. only append head changed)
            if (oldHead.writeHead == writeHead) {
                continue;
            }

            // Write head changed so retry with new head
            return oldHead;
        }

        return nHead;
    }
}

OrderedLogImpl::OrderedLogImpl(PageManager& pageManager)
        : BaseLogImpl(pageManager),
          mHead(acquirePage()),
          mTail(mHead.load()) {
    LOG_ASSERT(mHead.load() == mTail.load(), "Head and Tail do not point to the same page");
}

bool OrderedLogImpl::truncateLog(LogPage* oldTail, LogPage* newTail) {
    if (oldTail == newTail) {
        return (mTail.load() == newTail);
    }

    if (!mTail.compare_exchange_strong(oldTail, newTail)) {
        return false;
    }

    freePage(oldTail, newTail);

    return true;
}

LogEntry* OrderedLogImpl::appendEntry(uint32_t size, uint32_t entrySize, uint32_t type) {
    auto head = mHead.load();
    while (head) {
        // Try to append a new log entry to the page
        auto entry = head->appendEntry(size, entrySize, type);
        if (entry != nullptr) {
            return entry;
        }

        // The page must be full, acquire a new one
        head = createPage(head);
    }

    // This can only happen if the page manager ran out of space
    return nullptr;
}

LogPage* OrderedLogImpl::createPage(LogPage* oldHead) {
    // Check if the old head already has a next pointer
    auto next = oldHead->next().load();
    if (next) {
        // Try to set the next page as new head
        // If this fails then another thread already set a new head and oldHead will point to it
        if (!mHead.compare_exchange_strong(oldHead, next)) {
            return oldHead;
        }
        return next;
    }

    // Seal the old head so no one can append
    oldHead->seal();

    // Not enough space left in page - acquire new page
    auto nPage = acquirePage();
    if (!nPage) {
        LOG_ERROR("PageManager ran out of space");
        return nullptr;
    }

    // Try to set the new page as next page on the old head
    // If this fails then another thread already set a new page and next will point to it
    if (!oldHead->next().compare_exchange_strong(next, nPage)) {
        freeEmptyPageNow(nPage);
        return next;
    }

    // Set the page as new head
    // We do not care if this succeeds - if it does not, it means another thread updated the head for us
    mHead.compare_exchange_strong(oldHead, nPage);

    return nPage;
}

template <class Impl>
Log<Impl>::~Log() {
    // Safe Memory Reclamation mechanism has to ensure that the Log class is only deleted when no one references it
    // anymore so we can safely delete all pages here
    auto page = Impl::pageBegin();
    auto end = Impl::pageEnd();
    while (page != end) {
        auto next = page->next().load();
        Impl::freePageNow(page);
        page = next;
    }
}

template <class Impl>
LogEntry* Log<Impl>::append(uint32_t size, uint32_t type /* = 0x0u */) {
    auto entrySize = LogEntry::entrySizeFromSize(size);
    if (entrySize > LogPage::MAX_ENTRY_SIZE) {
        LOG_ASSERT(false, "Tried to append %d bytes but %d bytes is max", entrySize, LogPage::MAX_ENTRY_SIZE);
        return nullptr;
    }

    return Impl::appendEntry(size, entrySize, type);
}

template class Log<UnorderedLogImpl>;
template class Log<OrderedLogImpl>;

} // namespace store
} // namespace tell
