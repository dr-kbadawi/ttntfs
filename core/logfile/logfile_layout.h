/* SPDX-License-Identifier: GPL-2.0 */
/* Copyright (c) 2026 TechTag GmbH */
/*
 * On-disk layout of the NTFS $LogFile (the LFS journal) and of the NTFS
 * structures the replay engine touches (MFT records, attributes, index
 * blocks). Little-endian throughout; accessed via the get/put helpers so the
 * module never dereferences an unaligned or misaligned field.
 *
 * Sources: Linux fs/ntfs/logfile.h (Altaparmakov, restart pages), Linux
 * fs/ntfs3/fslog.c (Paragon, record pages/tables/ops), NTFS documentation
 * (Russon & Fledel). See docs/LOGFILE.md for what is verified vs inferred.
 */
#ifndef NTFS_LOGFILE_LAYOUT_H
#define NTFS_LOGFILE_LAYOUT_H

#include <stdint.h>
#include <stddef.h>
#include <string.h>

#define LFS_SECTOR_SIZE		512u
#define LFS_SECTOR_SHIFT	9u
#define LFS_MAX_FILE_SIZE	0x100000000ull
#define LFS_DEFAULT_PAGE_SIZE	4096u
#define LFS_MIN_RECORD_PAGES	0x30u

/* Record signatures. */
#define LFS_MAGIC_RSTR		0x52545352u	/* "RSTR" */
#define LFS_MAGIC_CHKD		0x444b4843u	/* "CHKD" */
#define LFS_MAGIC_RCRD		0x44524352u	/* "RCRD" */
#define LFS_MAGIC_FILE		0x454c4946u	/* "FILE" */
#define LFS_MAGIC_INDX		0x58444e49u	/* "INDX" */
#define LFS_MAGIC_BAAD		0x44414142u	/* "BAAD" */
#define LFS_MAGIC_HOLE		0x454c4f48u	/* "HOLE" */
#define LFS_MAGIC_EMPTY		0xffffffffu

/* Unaligned little-endian accessors. */
static inline uint16_t lf_get16(const void *p) { uint16_t v; memcpy(&v, p, 2); return v; }
static inline uint32_t lf_get32(const void *p) { uint32_t v; memcpy(&v, p, 4); return v; }
static inline uint64_t lf_get64(const void *p) { uint64_t v; memcpy(&v, p, 8); return v; }
static inline void lf_put16(void *p, uint16_t v) { memcpy(p, &v, 2); }
static inline void lf_put32(void *p, uint32_t v) { memcpy(p, &v, 4); }
static inline void lf_put64(void *p, uint64_t v) { memcpy(p, &v, 8); }
#define LF_ADD(p, n) ((void *)((uint8_t *)(p) + (n)))
#define LF_CADD(p, n) ((const void *)((const uint8_t *)(p) + (n)))
#define LF_ALIGN8(x) (((x) + 7u) & ~7u)

/* Generic multi-sector-protected record header (FILE/INDX/RSTR/RCRD). */
struct lfs_rec_hdr {
	uint32_t magic;		/* 0x00 */
	uint16_t usa_ofs;	/* 0x04 */
	uint16_t usa_count;	/* 0x06 */
	uint64_t lsn;		/* 0x08 */
};
#define LFS_REC_HDR_SIZE 0x10

/* Restart page header: 'RSTR' or 'CHKD'. */
enum {
	RP_MAGIC		= 0x00,
	RP_USA_OFS		= 0x04,
	RP_USA_COUNT		= 0x06,
	RP_CHKDSK_LSN		= 0x08,
	RP_SYSTEM_PAGE_SIZE	= 0x10,
	RP_LOG_PAGE_SIZE	= 0x14,
	RP_RESTART_AREA_OFFSET	= 0x18,
	RP_MINOR_VER		= 0x1a,
	RP_MAJOR_VER		= 0x1c,
	RP_HEADER_SIZE		= 0x1e,	/* usa follows immediately */
};

/* Restart area (at RP_RESTART_AREA_OFFSET). */
enum {
	RA_CURRENT_LSN		= 0x00,
	RA_LOG_CLIENTS		= 0x08,
	RA_CLIENT_FREE_LIST	= 0x0a,
	RA_CLIENT_IN_USE_LIST	= 0x0c,
	RA_FLAGS		= 0x0e,
	RA_SEQ_NUMBER_BITS	= 0x10,
	RA_RESTART_AREA_LENGTH	= 0x14,
	RA_CLIENT_ARRAY_OFFSET	= 0x16,
	RA_FILE_SIZE		= 0x18,
	RA_LAST_LSN_DATA_LENGTH	= 0x20,
	RA_LOG_RECORD_HEADER_LENGTH = 0x24,
	RA_LOG_PAGE_DATA_OFFSET	= 0x26,
	RA_RESTART_LOG_OPEN_COUNT = 0x28,
	RA_SIZE_V1		= 0x30,	/* Win2k client array at 0x30 */
	RA_SIZE_XP		= 0x40,	/* WinXP+ client array at 0x40 */
};
#define LFS_NO_CLIENT		0xffffu
#define RESTART_VOLUME_IS_CLEAN	0x0002u
#define RESTART_SINGLE_PAGE_IO	0x0001u

/* Log client record (0xa0 bytes). */
enum {
	CR_OLDEST_LSN		= 0x00,
	CR_CLIENT_RESTART_LSN	= 0x08,
	CR_PREV_CLIENT		= 0x10,
	CR_NEXT_CLIENT		= 0x12,
	CR_SEQ_NUMBER		= 0x14,
	CR_CLIENT_NAME_LENGTH	= 0x1c,
	CR_CLIENT_NAME		= 0x20,	/* 64 UTF-16 units */
	CR_SIZE			= 0xa0,
};

/* Log record page header: 'RCRD'. */
enum {
	PG_MAGIC		= 0x00,
	PG_USA_OFS		= 0x04,
	PG_USA_COUNT		= 0x06,
	PG_LAST_LSN		= 0x08,	/* v1: last LSN on page; tail copy: file offset */
	PG_FLAGS		= 0x10,	/* LFS_PAGE_LOG_RECORD_END */
	PG_PAGE_COUNT		= 0x14,
	PG_PAGE_POSITION	= 0x16,
	PG_NEXT_RECORD_OFFSET	= 0x18,
	PG_LAST_END_LSN		= 0x20,
	PG_USA			= 0x28,	/* v1.1 usa location */
	PG_FILE_OFFSET		= 0x3c,	/* v2.0 only */
};
#define LFS_PAGE_LOG_RECORD_END	0x00000001u

/* LFS record header (0x30 bytes) preceding every client record. */
enum {
	LR_THIS_LSN		= 0x00,
	LR_CLIENT_PREV_LSN	= 0x08,
	LR_CLIENT_UNDO_NEXT_LSN	= 0x10,
	LR_CLIENT_DATA_LENGTH	= 0x18,
	LR_CLIENT_SEQ_NUMBER	= 0x1c,
	LR_CLIENT_INDEX		= 0x1e,
	LR_RECORD_TYPE		= 0x20,
	LR_TRANSACTION_ID	= 0x24,
	LR_FLAGS		= 0x28,	/* LFS_RECORD_MULTI_PAGE */
	LR_HEADER_SIZE		= 0x30,
};
#define LFS_RECORD_TYPE_CLIENT		1u
#define LFS_RECORD_TYPE_CLIENT_RESTART	2u
#define LFS_RECORD_MULTI_PAGE		0x0001u

/* NTFS client log record header (0x20 bytes + page_lcns[]). */
enum {
	NR_REDO_OP		= 0x00,
	NR_UNDO_OP		= 0x02,
	NR_REDO_OFFSET		= 0x04,
	NR_REDO_LENGTH		= 0x06,
	NR_UNDO_OFFSET		= 0x08,
	NR_UNDO_LENGTH		= 0x0a,
	NR_TARGET_ATTRIBUTE	= 0x0c,
	NR_LCNS_TO_FOLLOW	= 0x0e,
	NR_RECORD_OFFSET	= 0x10,
	NR_ATTRIBUTE_OFFSET	= 0x12,
	NR_CLUSTER_BLOCK_OFFSET	= 0x14,	/* in 512-byte units */
	NR_RESERVED		= 0x16,
	NR_TARGET_VCN		= 0x18,
	NR_PAGE_LCNS		= 0x20,	/* u64[lcns_to_follow] */
	NR_HEADER_SIZE		= 0x20,
};

/* NTFS log operation codes. */
enum ntfs_log_op {
	LOP_Noop			= 0x00,
	LOP_CompensationLogRecord	= 0x01,
	LOP_InitializeFileRecordSegment	= 0x02,
	LOP_DeallocateFileRecordSegment	= 0x03,
	LOP_WriteEndOfFileRecordSegment	= 0x04,
	LOP_CreateAttribute		= 0x05,
	LOP_DeleteAttribute		= 0x06,
	LOP_UpdateResidentValue		= 0x07,
	LOP_UpdateNonresidentValue	= 0x08,
	LOP_UpdateMappingPairs		= 0x09,
	LOP_DeleteDirtyClusters		= 0x0a,
	LOP_SetNewAttributeSizes	= 0x0b,
	LOP_AddIndexEntryRoot		= 0x0c,
	LOP_DeleteIndexEntryRoot	= 0x0d,
	LOP_AddIndexEntryAllocation	= 0x0e,
	LOP_DeleteIndexEntryAllocation	= 0x0f,
	LOP_WriteEndOfIndexBuffer	= 0x10,
	LOP_SetIndexEntryVcnRoot	= 0x11,
	LOP_SetIndexEntryVcnAllocation	= 0x12,
	LOP_UpdateFileNameRoot		= 0x13,
	LOP_UpdateFileNameAllocation	= 0x14,
	LOP_SetBitsInNonresidentBitMap	= 0x15,
	LOP_ClearBitsInNonresidentBitMap = 0x16,
	LOP_HotFix			= 0x17,
	LOP_EndTopLevelAction		= 0x18,
	LOP_PrepareTransaction		= 0x19,
	LOP_CommitTransaction		= 0x1a,
	LOP_ForgetTransaction		= 0x1b,
	LOP_OpenNonresidentAttribute	= 0x1c,
	LOP_OpenAttributeTableDump	= 0x1d,
	LOP_AttributeNamesDump		= 0x1e,
	LOP_DirtyPageTableDump		= 0x1f,
	LOP_TransactionTableDump	= 0x20,
	LOP_UpdateRecordDataRoot	= 0x21,
	LOP_UpdateRecordDataAllocation	= 0x22,
	LOP_UpdateRelativeDataInIndex	= 0x23,
	LOP_UpdateRelativeDataInIndex2	= 0x24,
	LOP_ZeroEndOfFileRecord		= 0x25,
	LOP__MAX			= 0x26,
};

/* Restart table header (0x18 bytes), used for the transaction, dirty page
 * and open attribute tables. Entries follow; an allocated entry starts with
 * u32 0xFFFFFFFF, a free one with the offset of the next free entry. */
enum {
	RT_ENTRY_SIZE		= 0x00,
	RT_NUMBER_ENTRIES	= 0x02,	/* "used" in ntfs3 */
	RT_NUMBER_ALLOCATED	= 0x04,	/* "total" */
	RT_FREE_GOAL		= 0x0c,
	RT_FIRST_FREE		= 0x10,
	RT_LAST_FREE		= 0x14,
	RT_HEADER_SIZE		= 0x18,
};
#define RT_ENTRY_ALLOCATED	0xffffffffu

/* NTFS_RESTART: client restart record data (0x40 bytes). */
enum {
	CRST_MAJOR_VER		= 0x00,
	CRST_MINOR_VER		= 0x04,
	CRST_START_OF_CHECKPOINT = 0x08,
	CRST_OPEN_ATTR_TABLE_LSN = 0x10,
	CRST_ATTR_NAMES_LSN	= 0x18,
	CRST_DIRTY_PAGE_TABLE_LSN = 0x20,
	CRST_TRANSACTION_TABLE_LSN = 0x28,
	CRST_OPEN_ATTR_TABLE_LEN = 0x30,
	CRST_ATTR_NAMES_LEN	= 0x34,
	CRST_DIRTY_PAGE_TABLE_LEN = 0x38,
	CRST_TRANSACTION_TABLE_LEN = 0x3c,
	CRST_SIZE		= 0x40,
};

/* Open attribute entry, restart version 1 (0x28 bytes). */
enum {
	OA1_NEXT		= 0x00,
	OA1_BYTES_PER_INDEX	= 0x04,
	OA1_TYPE		= 0x08,
	OA1_DIRTY_PAGES		= 0x0c,
	OA1_FILE_REF		= 0x10,	/* MFT_REF */
	OA1_OPEN_RECORD_LSN	= 0x18,
	OA1_POINTER		= 0x20,	/* in-memory only on disk; ignored */
	OA1_SIZE		= 0x28,
};
/* Open attribute entry, restart version 0 (0x2c bytes). */
enum {
	OA0_NEXT		= 0x00,
	OA0_POINTER		= 0x04,
	OA0_FILE_REF		= 0x08,
	OA0_OPEN_RECORD_LSN	= 0x10,
	OA0_DIRTY_PAGES		= 0x18,
	OA0_TYPE		= 0x1c,
	OA0_NAME_LEN		= 0x20,
	OA0_BYTES_PER_INDEX	= 0x28,
	OA0_SIZE		= 0x2c,
};

/* Attribute name entry (variable): u16 open-attr offset, u16 name bytes, name. */
enum { AN_OFFSET = 0x00, AN_NAME_BYTES = 0x02, AN_NAME = 0x04 };

/* Dirty page entry, version 1 (0x20 bytes + u64 lcns[]). */
enum {
	DP1_NEXT		= 0x00,
	DP1_TARGET_ATTR		= 0x04,
	DP1_TRANSFER_LEN	= 0x08,
	DP1_LCNS_FOLLOW		= 0x0c,
	DP1_VCN			= 0x10,
	DP1_OLDEST_LSN		= 0x18,
	DP1_PAGE_LCNS		= 0x20,
	DP1_SIZE		= 0x20,
};
/* Dirty page entry, version 0 (0x2c bytes header). */
enum {
	DP0_NEXT		= 0x00,
	DP0_TARGET_ATTR		= 0x04,
	DP0_TRANSFER_LEN	= 0x08,
	DP0_LCNS_FOLLOW		= 0x0c,
	DP0_VCN			= 0x14,
	DP0_OLDEST_LSN		= 0x1c,
	DP0_PAGE_LCNS		= 0x24,
};

/* Transaction entry (0x28 bytes). */
enum {
	TR_NEXT			= 0x00,
	TR_STATE		= 0x04,
	TR_FIRST_LSN		= 0x08,
	TR_PREV_LSN		= 0x10,
	TR_UNDO_NEXT_LSN	= 0x18,
	TR_UNDO_RECORDS		= 0x20,
	TR_UNDO_BYTES		= 0x24,
	TR_SIZE			= 0x28,
};
enum {
	TRANSACTION_UNINITIALIZED = 0,
	TRANSACTION_ACTIVE,
	TRANSACTION_PREPARED,
	TRANSACTION_COMMITTED,
};

/* Payload of SetNewAttributeSizes (0x20 bytes). */
enum { NAS_ALLOC = 0x00, NAS_VALID = 0x08, NAS_DATA = 0x10, NAS_TOTAL = 0x18, NAS_SIZE = 0x20 };
/* Payload of Set/ClearBitsInNonresidentBitMap. */
enum { BR_BITMAP_OFF = 0x00, BR_BITS = 0x04, BR_SIZE = 0x08 };
/* Payload of DeleteDirtyClusters: array of { u64 lcn, u64 len }. */
enum { LR_RANGE_SIZE = 0x10 };

/* ---- NTFS structures touched by replay -------------------------------- */

/* MFT record header. */
enum {
	MR_MAGIC		= 0x00,
	MR_USA_OFS		= 0x04,
	MR_USA_COUNT		= 0x06,
	MR_LSN			= 0x08,
	MR_SEQUENCE		= 0x10,
	MR_LINK_COUNT		= 0x12,
	MR_ATTRS_OFFSET		= 0x14,
	MR_FLAGS		= 0x16,
	MR_BYTES_IN_USE		= 0x18,
	MR_BYTES_ALLOCATED	= 0x1c,
	MR_BASE_RECORD		= 0x20,	/* MFT_REF */
	MR_NEXT_ATTR_ID		= 0x28,
	MR_RESERVED		= 0x2a,
	MR_RECORD_NUMBER	= 0x2c,
	MR_FIXUP_OFFSET_1	= 0x2a,	/* NT4 usa position */
	MR_FIXUP_OFFSET_3	= 0x30,	/* XP+ usa position */
};
#define MFT_RECORD_IN_USE	0x0001u
#define MFT_RECORD_IS_DIRECTORY	0x0002u

/* Attribute record header. */
enum {
	AT_TYPE			= 0x00,
	AT_LENGTH		= 0x04,
	AT_NON_RESIDENT		= 0x08,
	AT_NAME_LENGTH		= 0x09,
	AT_NAME_OFFSET		= 0x0a,
	AT_FLAGS		= 0x0c,
	AT_INSTANCE		= 0x0e,
	/* resident */
	AT_VALUE_LENGTH		= 0x10,
	AT_VALUE_OFFSET		= 0x14,
	AT_RES_FLAGS		= 0x16,
	AT_RESIDENT_SIZE	= 0x18,
	/* non-resident */
	AT_LOWEST_VCN		= 0x10,
	AT_HIGHEST_VCN		= 0x18,
	AT_MAPPING_PAIRS_OFFSET	= 0x20,
	AT_COMPRESSION_UNIT	= 0x22,
	AT_ALLOCATED_SIZE	= 0x28,
	AT_DATA_SIZE		= 0x30,
	AT_INITIALIZED_SIZE	= 0x38,
	AT_COMPRESSED_SIZE	= 0x40,
	AT_NONRESIDENT_SIZE	= 0x40,
	AT_NONRESIDENT_EX_SIZE	= 0x48,
};
#define ATTR_TYPE_STANDARD_INFORMATION	0x10u
#define ATTR_TYPE_ATTRIBUTE_LIST	0x20u
#define ATTR_TYPE_FILE_NAME		0x30u
#define ATTR_TYPE_OBJECT_ID		0x40u
#define ATTR_TYPE_SECURITY_DESCRIPTOR	0x50u
#define ATTR_TYPE_VOLUME_NAME		0x60u
#define ATTR_TYPE_VOLUME_INFORMATION	0x70u
#define ATTR_TYPE_DATA			0x80u
#define ATTR_TYPE_INDEX_ROOT		0x90u
#define ATTR_TYPE_INDEX_ALLOCATION	0xa0u
#define ATTR_TYPE_BITMAP		0xb0u
#define ATTR_TYPE_REPARSE_POINT		0xc0u
#define ATTR_TYPE_EA_INFORMATION	0xd0u
#define ATTR_TYPE_EA			0xe0u
#define ATTR_TYPE_PROPERTY_SET		0xf0u
#define ATTR_TYPE_LOGGED_UTILITY_STREAM	0x100u
#define ATTR_TYPE_END			0xffffffffu
#define ATTR_FLAG_COMPRESSED		0x0001u
#define ATTR_FLAG_ENCRYPTED		0x4000u
#define ATTR_FLAG_SPARSE		0x8000u
#define ATTR_RES_FLAG_INDEXED		0x01u

/* Attribute list entry. */
enum {
	AL_TYPE			= 0x00,
	AL_LENGTH		= 0x04,
	AL_NAME_LENGTH		= 0x06,
	AL_NAME_OFFSET		= 0x07,
	AL_LOWEST_VCN		= 0x08,
	AL_MFT_REFERENCE	= 0x10,
	AL_INSTANCE		= 0x18,
	AL_NAME			= 0x1a,
};

/* Index root / header / entry / block. */
enum {
	IR_TYPE			= 0x00,
	IR_COLLATION_RULE	= 0x04,
	IR_INDEX_BLOCK_SIZE	= 0x08,
	IR_CLUSTERS_PER_BLOCK	= 0x0c,
	IR_INDEX_HEADER		= 0x10,
};
enum {
	IH_ENTRIES_OFFSET	= 0x00,
	IH_INDEX_LENGTH		= 0x04,	/* "used" */
	IH_ALLOCATED_SIZE	= 0x08,	/* "total" */
	IH_FLAGS		= 0x0c,
	IH_SIZE			= 0x10,
};
#define INDEX_HDR_HAS_SUBNODES	0x01u
enum {
	IE_KEY_REF		= 0x00,	/* MFT_REF for $I30; data_off/data_size view otherwise */
	IE_DATA_OFFSET		= 0x00,
	IE_DATA_LENGTH		= 0x02,
	IE_LENGTH		= 0x08,
	IE_KEY_LENGTH		= 0x0a,
	IE_FLAGS		= 0x0c,
	IE_KEY			= 0x10,
	IE_MIN_SIZE		= 0x10,
};
#define INDEX_ENTRY_NODE	0x01u
#define INDEX_ENTRY_END		0x02u
enum {
	IB_MAGIC		= 0x00,
	IB_USA_OFS		= 0x04,
	IB_USA_COUNT		= 0x06,
	IB_LSN			= 0x08,
	IB_INDEX_BLOCK_VCN	= 0x10,
	IB_INDEX_HEADER		= 0x18,
};
/* FILE_NAME attribute: parent ref (8) then NTFS_DUP_INFO (0x38) then lengths. */
#define FN_DUP_OFFSET		0x08
#define FN_DUP_SIZE		0x38

#endif /* NTFS_LOGFILE_LAYOUT_H */
