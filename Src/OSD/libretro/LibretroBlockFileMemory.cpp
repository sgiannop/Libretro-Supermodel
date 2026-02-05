#include "LibretroBlockFileMemory.h"

// ==============================
// CBlockFileCounter
// ==============================


CBlockFileCounter::CBlockFileCounter()
    : m_size(0)
{
}

size_t CBlockFileCounter::GetSize() const
{
    return m_size;
}

void CBlockFileCounter::Write(const void* data, uint32_t numBytes)
{
    m_size += numBytes;
}

void CBlockFileCounter::Write(bool /*value*/)
{
    m_size += 1; // bool stored as 1 byte in BlockFile
}

void CBlockFileCounter::Write(const std::string& str)
{
    // BlockFile writes string INCLUDING null terminator
    m_size += str.size() + 1;
}


    // Read stubs for the counter
unsigned Read(void *data, uint32_t numBytes)  { return 0; }
unsigned Read(bool *value)  { return 0; }

// ==============================
// CBlockFileMemory
// ==============================

CBlockFileMemory::CBlockFileMemory(void* data, size_t size)
    : m_ptr(static_cast<uint8_t*>(data))
    , m_offset(0)
    , m_capacity(size)
{
}

Result CBlockFileMemory::FindBlock(const std::string &name)
{
    size_t searchOffset = 0; // Local search pointer

    while (searchOffset + 12 <= m_capacity)
    {
        size_t blockStart = searchOffset;

        uint32_t totalBlockLength, nameLen, commentLen;
        std::memcpy(&totalBlockLength, m_ptr + searchOffset, 4); searchOffset += 4;
        std::memcpy(&nameLen, m_ptr + searchOffset, 4);          searchOffset += 4;
        std::memcpy(&commentLen, m_ptr + searchOffset, 4);       searchOffset += 4;

        if (nameLen == 0 || nameLen > 2048 || searchOffset + nameLen > m_capacity)
            break;

        std::string blockName(reinterpret_cast<char*>(m_ptr + searchOffset));
        
        // Is this our block?
        if (blockName == name)
        {
            // Set the GLOBAL offset to the start of the DATA for this block
            // Data starts after headers (12) + name + comment
            m_offset = blockStart + 12 + nameLen + commentLen; 
            return Result::OKAY;
        }

        // If not, we MUST skip to the next block correctly.
        // Supermodel blocks are self-describing.
        if (totalBlockLength > 0)
            searchOffset = blockStart + totalBlockLength;
        else {
            // If totalBlockLength is 0 (which our Save currently writes), 
            // we have a problem: we don't know where the data ends.
            // But wait—Supermodel only writes ONE block usually! 
            // If there are multiple, we MUST fix the Save to write the length.
            break; 
        }
    }
    return Result::FAIL;
}

void CBlockFileMemory::Write(const void* data, uint32_t numBytes)
{
    if (m_offset + numBytes > m_capacity)
        return; // or assert

    std::memcpy(m_ptr + m_offset, data, numBytes);
    m_offset += numBytes;
}

void CBlockFileMemory::Write(bool value)
{
    uint8_t v = value ? 1 : 0;
    Write(&v, 1);
}

void CBlockFileMemory::Write(const std::string& str)
{
    Write(str.c_str(), static_cast<uint32_t>(str.size() + 1));
}

void CBlockFileCounter::NewBlock(const std::string& name, const std::string& comment) {
    m_size += 12 + (name.size() + 1) + (comment.size() + 1);
}

// --- READING (Crucial for retro_unserialize) ---
unsigned CBlockFileMemory::Read(void *data, uint32_t numBytes)  {
    if (m_offset + numBytes > m_capacity) numBytes = m_capacity - m_offset;
    std::memcpy(data, m_ptr + m_offset, numBytes);
    m_offset += numBytes;
    return numBytes;
}

unsigned CBlockFileMemory::Read(bool *value)  {
    uint8_t v;
    unsigned read = Read(&v, 1);
    if (read == 1) *value = (v != 0);
    return read;
}

// ADDED THIS: The emulator needs this to succeed to keep saving!
void CBlockFileMemory::NewBlock(const std::string& name, const std::string& comment) {
    // 1. If there was a previous block, update its total length field
    if (m_offset > 0 && m_currentBlockHeaderPos < m_capacity) {
        uint32_t totalSize = static_cast<uint32_t>(m_offset - m_currentBlockHeaderPos);
        std::memcpy(m_ptr + m_currentBlockHeaderPos, &totalSize, 4);
    }

    // 2. Record where this new block's length field is located
    m_currentBlockHeaderPos = m_offset;

    // 3. Write the header
    uint32_t dummyTotalLen = 0;
    uint32_t nLen = (uint32_t)name.size() + 1;
    uint32_t cLen = (uint32_t)comment.size() + 1;

    Write(&dummyTotalLen, 4);
    Write(&nLen, 4);
    Write(&cLen, 4);
    Write(name.c_str(), nLen);
    Write(comment.c_str(), cLen);
}

void CBlockFileMemory::Finish() {
    if (m_offset > 0) {
        uint32_t totalSize = static_cast<uint32_t>(m_offset - m_currentBlockHeaderPos);
        std::memcpy(m_ptr + m_currentBlockHeaderPos, &totalSize, 4);
    }
}