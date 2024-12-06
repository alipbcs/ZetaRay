#include "../App/Filesystem.h"

namespace ZetaRay::App::Filesystem
{
    template<Support::AllocatorType Allocator, uint32_t InlineStorageLength>
    struct FilePath
    {
        explicit FilePath(const Allocator& a = Allocator())
            : m_path(a)
        {}
        explicit FilePath(Util::StrView str, const Allocator& a = Allocator())
            : m_path(a)
        {
            const size_t n = str.size();
            m_path.resize(n + 1);   // + 1 for '\0'

            if (n)
                memcpy(m_path.data(), str.data(), n);

            m_path[n] = '\0';
        }
        FilePath(const FilePath&) = delete;
        FilePath& operator=(const FilePath&) = delete;

        ZetaInline bool IsEmpty() const { return m_path.empty(); }
        ZetaInline void Resize(size_t n) { m_path.resize(n); };
        ZetaInline char* Get() { return m_path.begin(); }
        ZetaInline const char* Get() const { return m_path.begin(); }
        ZetaInline Util::StrView GetView() const { return Util::StrView(m_path.begin(), m_path.size()); }
        ZetaInline size_t Length() const { return m_path.size(); }
        ZetaInline bool HasInlineStorage() const { return m_path.has_inline_storage(); }

        void Reset(Util::StrView str)
        {
            if (!str.empty())
            {
                const size_t n = str.size();
                m_path.resize(n + 1);   // + 1 for '\0'
                memcpy(m_path.data(), str.data(), n);
                m_path[n] = '\0';
            }
        }

        FilePath<Allocator, InlineStorageLength>& Append(Util::StrView str, bool useBackslash = true)
        {
            if (str.empty())
                return *this;

            // don't read uninitialized memory
            // (Note: underlying path storage's size and the actual string length may not match)
            const size_t curr = m_path.empty() ? 0 : strlen(m_path.data());

            size_t additionLen = str.size();
            Util::SmallVector<char, Support::SystemAllocator, InlineStorageLength> toAppend;
            toAppend.resize(additionLen + 1);

            if (curr)
            {
                toAppend[0] = useBackslash ? '\\' : '/';
                memcpy(toAppend.data() + 1, str.data(), additionLen);
                additionLen++;
            }
            else
                memcpy(toAppend.data(), str.data(), additionLen);

            const size_t newSize = curr + additionLen + 1;
            m_path.resize(newSize);

            memcpy(m_path.begin() + curr, toAppend.data(), additionLen);
            m_path[curr + additionLen] = '\0';

            return *this;
        }

        FilePath<Allocator, InlineStorageLength>& ToParent()
        {
            const size_t len = strlen(m_path.data());

            char* beg = m_path.begin();
            char* curr = beg + len;

            while (curr >= beg && *curr != '\\' && *curr != '/')
                curr--;

            if (*curr == '\\' || *curr == '/')
                *curr = '\0';
            else
            {
                m_path.resize(3);

                m_path[0] = '.';
                m_path[1] = '.';
                m_path[2] = '\0';
            }

            return *this;
        }

        FilePath<Allocator, InlineStorageLength>& Directory()
        {
            if (Filesystem::IsDirectory(m_path.data()))
                return *this;

            const size_t len = strlen(m_path.data());
            char* beg = m_path.begin();
            char* curr = beg + len;

            while (curr >= beg && *curr != '\\' && *curr != '/')
                curr--;

            if (*curr == '\\' || *curr == '/')
                *curr = '\0';
            else
            {
                m_path.resize(2);
                m_path[0] = '.';
                m_path[1] = '\0';
            }

            return *this;
        }

        void Stem(Util::MutableSpan<char> buff, size_t* outStrLen = nullptr) const
        {
            const size_t len = strlen(m_path.begin());

            char* beg = const_cast<char*>(m_path.begin());
            char* curr = beg + len - 1;

            size_t start = size_t(-1);
            size_t end = size_t(-1);
            char* firstDot = nullptr;

            // figure out the first ., e.g. a.b.c -> a
            while (curr >= beg && *curr != '\\' && *curr != '/')
            {
                if (*curr == '.')
                    firstDot = curr;

                curr--;
            }

            firstDot = firstDot ? firstDot : beg + len;
            end = firstDot - beg;
            start = curr - beg + 1;

            size_t s = end - start + 1;
            //Check(buff.size() >= s, "provided buffer is too small");

            if (s > 1)
                memcpy(buff.data(), beg + start, Math::Min(s - 1, buff.size() - 1));

            buff.data()[s - 1] = '\0';

            if (outStrLen)
                *outStrLen = s - 1;
        }

        void Extension(Util::MutableSpan<char> buff, size_t* outStrLen = nullptr) const
        {
            const size_t len = strlen(m_path.begin());

            char* beg = const_cast<char*>(m_path.begin());
            char* curr = beg + len - 1;

            size_t start = size_t(-1);
            size_t end = size_t(-1);

            // figure out the first ., e.g. a.b.c -> a
            while (curr >= beg && *curr != '.')
                curr--;

            if (curr < beg)
            {
                if (outStrLen)
                    *outStrLen = 0;

                return;
            }

            size_t s = beg + len - 1 - curr;
            Check(buff.size() >= s + 1, "Provided buffer is too small.");

            if (s > 1)
                memcpy(buff.data(), curr + 1, s);

            buff.data()[s] = '\0';

            if (outStrLen)
                *outStrLen = s;
        }

        void ConvertToBackslashes()
        {
            const size_t len = strlen(m_path.begin());
            char* srcData = m_path.begin();

            for (size_t i = 0; i < len; i++)
            {
                if (srcData[i] == '/')
                    srcData[i] = '\\';
            }
        }

        void ConvertToForwardSlashes()
        {
            const size_t len = strlen(m_path.begin());
            char* srcData = m_path.begin();

            for (size_t i = 0; i < len; i++)
            {
                if (srcData[i] == '\\')
                    srcData[i] = '/';
            }
        }

    private:
        Util::SmallVector<char, Allocator, InlineStorageLength> m_path;
    };

    using Path = FilePath<Support::SystemAllocator, 128>;
}