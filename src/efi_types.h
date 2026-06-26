#pragma once
#define WIN32_LEAN_AND_MEAN
#define UNICODE
#define _UNICODE
#include <windows.h>
#include <stdint.h>
#include <string>
#include <cstring>

#pragma pack(push, 1)

// ─── GUID ───────────────────────────────────────────────────────────────────

struct EFI_GUID {
    uint32_t Data1;
    uint16_t Data2;
    uint16_t Data3;
    uint8_t  Data4[8];

    bool operator==(const EFI_GUID& o) const { return memcmp(this, &o, 16) == 0; }
    bool operator!=(const EFI_GUID& o) const { return !(*this == o); }

    std::wstring ToString() const {
        wchar_t buf[64];
        swprintf_s(buf, 64,
            L"{%08X-%04X-%04X-%02X%02X-%02X%02X%02X%02X%02X%02X}",
            Data1, Data2, Data3,
            Data4[0], Data4[1],
            Data4[2], Data4[3], Data4[4], Data4[5], Data4[6], Data4[7]);
        return buf;
    }
};

// EFI Global Variable namespace
// {8BE4DF61-93CA-11D2-AA0D-00E098032B8C}
static const EFI_GUID EFI_GLOBAL_VARIABLE_GUID = {
    0x8BE4DF61, 0x93CA, 0x11D2,
    {0xAA, 0x0D, 0x00, 0xE0, 0x98, 0x03, 0x2B, 0x8C}
};

// EFI System Partition type GUID
// {C12A7328-F81F-11D2-BA4B-00A0C93EC93B}
static const EFI_GUID ESP_PARTITION_TYPE_GUID = {
    0xC12A7328, 0xF81F, 0x11D2,
    {0xBA, 0x4B, 0x00, 0xA0, 0xC9, 0x3E, 0xC9, 0x3B}
};

// Microsoft Basic Data partition type GUID
// {EBD0A0A2-B9E5-4433-87C0-68B6B72699C7}
static const EFI_GUID MICROSOFT_BASIC_DATA_GUID = {
    0xEBD0A0A2, 0xB9E5, 0x4433,
    {0x87, 0xC0, 0x68, 0xB6, 0xB7, 0x26, 0x99, 0xC7}
};

// ─── DEVICE PATH ────────────────────────────────────────────────────────────

#define HARDWARE_DEVICE_PATH        0x01
#define ACPI_DEVICE_PATH            0x02
#define MESSAGING_DEVICE_PATH       0x03
#define MEDIA_DEVICE_PATH           0x04
#define BBS_DEVICE_PATH             0x05
#define END_DEVICE_PATH_TYPE        0x7F

#define MEDIA_HARDDRIVE_DP          0x01
#define MEDIA_CDROM_DP              0x02
#define MEDIA_FILEPATH_DP           0x04
#define END_ENTIRE_DEVICE_PATH      0xFF

#define MBR_TYPE_MBR                0x01
#define MBR_TYPE_GPT                0x02

#define SIGNATURE_TYPE_NONE         0x00
#define SIGNATURE_TYPE_MBR          0x01
#define SIGNATURE_TYPE_GUID         0x02

struct EFI_DEVICE_PATH_PROTOCOL {
    uint8_t  Type;
    uint8_t  SubType;
    uint16_t Length;   // Total length including this 4-byte header
};

// HARDDRIVE_DEVICE_PATH = 42 bytes total
struct HARDDRIVE_DEVICE_PATH {
    EFI_DEVICE_PATH_PROTOCOL Header;   //  4 bytes  Type=0x04, SubType=0x01, Length=42
    uint32_t PartitionNumber;          //  4 bytes  1-based
    uint64_t PartitionStart;           //  8 bytes  LBA
    uint64_t PartitionSize;            //  8 bytes  LBAs
    uint8_t  Signature[16];            // 16 bytes  GPT: UniquePartitionGUID
    uint8_t  MBRType;                  //  1 byte   0x02 = GPT
    uint8_t  SignatureType;            //  1 byte   0x02 = GUID
};                                     // Total = 42 bytes

static_assert(sizeof(HARDDRIVE_DEVICE_PATH) == 42, "Wrong HARDDRIVE_DEVICE_PATH size");

struct FILEPATH_DEVICE_PATH_HEADER {
    EFI_DEVICE_PATH_PROTOCOL Header;   // Type=0x04, SubType=0x04
    // Followed immediately by null-terminated UTF-16LE path
};

// END device path node (4 bytes)
struct END_DEVICE_PATH_NODE {
    uint8_t  Type;     // 0x7F
    uint8_t  SubType;  // 0xFF
    uint16_t Length;   // 4
};

// ─── GPT ────────────────────────────────────────────────────────────────────

#define GPT_HEADER_SIGNATURE_VALUE  0x5452415020494645ULL  // "EFI PART"
#define GPT_HEADER_REVISION_1_0     0x00010000

struct GPT_HEADER {
    uint64_t Signature;
    uint32_t Revision;
    uint32_t HeaderSize;
    uint32_t HeaderCRC32;
    uint32_t Reserved;
    uint64_t MyLBA;
    uint64_t AlternateLBA;
    uint64_t FirstUsableLBA;
    uint64_t LastUsableLBA;
    EFI_GUID DiskGUID;
    uint64_t PartitionEntryLBA;
    uint32_t NumberOfPartitionEntries;
    uint32_t SizeOfPartitionEntry;
    uint32_t PartitionEntryArrayCRC32;
};

struct GPT_PARTITION_ENTRY {
    EFI_GUID PartitionTypeGUID;
    EFI_GUID UniquePartitionGUID;
    uint64_t StartingLBA;
    uint64_t EndingLBA;
    uint64_t Attributes;
    wchar_t  PartitionName[36];   // UTF-16LE, null-padded
};

// ─── EFI LOAD OPTION ────────────────────────────────────────────────────────

#define LOAD_OPTION_ACTIVE              0x00000001
#define LOAD_OPTION_FORCE_RECONNECT     0x00000002
#define LOAD_OPTION_HIDDEN              0x00000008
#define LOAD_OPTION_CATEGORY_BOOT       0x00000000
#define LOAD_OPTION_CATEGORY_APP        0x00000100

// EFI_LOAD_OPTION binary layout (variable size):
//   uint32_t  Attributes
//   uint16_t  FilePathListLength
//   wchar_t   Description[]     (null-terminated UTF-16)
//   uint8_t   FilePathList[]    (FilePathListLength bytes)
//   uint8_t   OptionalData[]    (remainder, may be empty)

#pragma pack(pop)
