#include <hkxparse/HKXTagfileParser.h>
#include <hkxparse/HKXMapping.h>
#include <hkxparse/TagfileTypes.h>

#include <sstream>
#include <stdexcept>
#include <array>
#include <cstdio>

// monkey_dust: upstream leaves per-field debug tracing on unconditionally.
// On the full 3995-tile Kenshi navmesh dataset this produces tens of millions
// of lines (~2GB+ for a partial run) -- generating/writing that much text is
// what was actually destabilizing the sandbox on batch runs, not a parser
// hang or memory leak. Gated off by default; flip to 1 only for single-file
// debugging.
#ifndef HKX_TRACE
#define HKX_TRACE 0
#endif
#if HKX_TRACE
#define HKX_TRACE_PRINTF(...) printf(__VA_ARGS__)
#define HKX_TRACE_FPRINTF(...) fprintf(__VA_ARGS__)
#else
#define HKX_TRACE_PRINTF(...) do {} while (0)
#define HKX_TRACE_FPRINTF(...) do {} while (0)
#endif

namespace hkxparse {
	HKXTagfileParser::HKXTagfileParser(HKXMapping &mapping) : m_mapping(mapping), m_nextAllocatedObject(1) {
		m_rules.bytesInPointer = 0;
		
		const auto &header = *reinterpret_cast<TagfileHeader *>(m_mapping.data());
		if (header.magic0 == TagfileMagic0 && header.magic1 == TagfileMagic1) {
			m_rules.littleEndian = 1;
		}
		else if ((header.magic0 == _byteswap_ulong(TagfileMagic0) && header.magic1 == _byteswap_ulong(TagfileMagic1))) {
			m_rules.littleEndian = 0;
		}
		else {
			throw std::runtime_error("bad tagfile magic");
		}

		m_rules.reusePaddingOptimization = 0;
		m_rules.emptyBaseClassOptimization = 0;

		m_stream = Deserializer(m_rules, m_mapping.data() + sizeof(TagfileHeader), m_mapping.size() - sizeof(TagfileHeader));

		TagfileTypeInfo voidType;
		voidType.name = "BuiltinVoidType";
		voidType.parentTypeIndex = 0;
		voidType.unk3 = 0;

		TagfileMemberInfo voidTypeMember;
		voidTypeMember.name = "void";
		voidTypeMember.type = TagTypeVoid;
		voidType.members.emplace_back(std::move(voidTypeMember));
		m_types.emplace_back(std::move(voidType));
	}
	
	HKXTagfileParser::~HKXTagfileParser() {

	}

	HKXStructRef HKXTagfileParser::parse() {
		while (true) {
			auto offsetBefore = m_stream.currentPtr() - (m_mapping.data() + sizeof(TagfileHeader));
			auto type = m_stream.readVarInt();
			HKX_TRACE_FPRINTF(stderr, "[trace] top-level tag=%d (offset=%ld raw=%02x %02x %02x %02x)\n",
			        (int)type, (long)offsetBefore,
			        m_mapping.data()[sizeof(TagfileHeader)+offsetBefore+0],
			        m_mapping.data()[sizeof(TagfileHeader)+offsetBefore+1],
			        m_mapping.data()[sizeof(TagfileHeader)+offsetBefore+2],
			        m_mapping.data()[sizeof(TagfileHeader)+offsetBefore+3]);

			switch (type) {
			case TagFileInfo:
			{
				// monkey_dust: a real tagfile has exactly one TagFileInfo, at
				// the very start. A later coincidental tag byte that happens
				// to decode to 1 is garbage (confirmed: on tile8.4.hkt et al.
				// this occurred deep in trailing per-navmesh data and, when
				// treated as a real header, read a bogus version-string length
				// straight into an uncaught "out of bounds read"). Treat any
				// non-first occurrence exactly like an unhandled tag.
				if (m_sawFileInfo) {
					if (tryResync((size_t)offsetBefore)) break;
					throw std::runtime_error("unexpected duplicate TagFileInfo tag");
				}
				m_sawFileInfo = true;

				auto version = m_stream.readVarInt();
				HKX_TRACE_FPRINTF(stderr, "[trace] TagFileInfo version=%d\n", (int)version);

				if (version == 3 || version == 4 || version == 5) {
					m_stringPool.clear();
					m_stringPool.emplace_back();
					m_stringPool.emplace_back();
				}

				switch (version) {
				// monkey_dust: REVERTED case 1 -- multi-file test proved this was
				// coincidental garbage, not a real second TagFileInfo section
				// (tile5.5 hit "version=553" at the analogous position, tile10.10
				// didn't hit this path at all). The real bug is upstream, inside
				// hkaiNavMesh's own field parsing (likely faceData/edgeData array
				// length, or vertices' Vec3-packed prefix mechanism).
				case 3:
					break;

				case 4:
					m_havokVersion = readString();
					break;

				case 5: {
					// monkey_dust: empirically determined against tile0.0.hkt --
					// v5 reads the havok version string like v4, THEN has 6 extra
					// bytes (3 more varints, values observed: -10, 0, 0) before the
					// next real tag. Byte offsets verified by hand against the raw
					// hex dump: the TagMetadata(=2) marker byte (0x04) and the
					// following 20-byte "hkRootLevelContainer" string only land on
					// the correct offsets if exactly 6 extra single-byte varints
					// are consumed here (observed: -10, 0, 0, 0, 10, 0 -- all with
					// continuation bit clear). Meaning NOT understood (perhaps a
					// contents-version tuple / GUID-like field) -- only the byte
					// alignment is verified against the real file.
					m_havokVersion = readString();
					HKX_TRACE_FPRINTF(stderr, "[trace] havokVersion=%s\n", m_havokVersion.c_str());
					int32_t extra[6];
					for (auto &e : extra) e = m_stream.readVarInt();
					HKX_TRACE_FPRINTF(stderr, "[trace] v5 extra fields: %d %d %d %d %d %d\n",
					        (int)extra[0], (int)extra[1], (int)extra[2], (int)extra[3], (int)extra[4], (int)extra[5]);
					break;
				}

				default:
				{
					// monkey_dust: same recovery heuristic as the outer switch's
					// default case (see tryResync()) -- an unrecognized
					// TagFileInfo version turned out to just be more of the
					// same "undocumented trailing data" pattern seen after
					// hkaiNavMesh, not a real distinct format version.
					if (tryResync((size_t)offsetBefore)) break;

					std::stringstream error;
					error << "Unsupported version " << version;
					throw std::runtime_error(error.str());
				}
				}

				break;
			}

			case TagMetadata:
			{
				// monkey_dust: TagMetadata(2) is ALWAYS a handled case, so a
				// coincidental garbage byte that happens to decode to 2 never
				// reaches the default-case resync at all -- confirmed in
				// practice (a byte deep in "mystery" trailing data decoded to
				// tag=2, and readTypeInfo() read garbled UTF-8 as a type/member
				// name, eventually throwing "out of bounds read" much later).
				// Validate the parsed type BEFORE committing it to m_types;
				// on failure, treat this exactly like an unhandled tag and
				// resync from the tag's own position.
				auto savedStringPool = m_stringPool;
				TagfileTypeInfo info;
				bool sane = true;
				try {
					info = readTypeInfo();
					if (info.name.empty() || info.name.size() > 128) sane = false;
					for (unsigned char c : info.name) {
						if (c < 0x20 || c > 0x7E) { sane = false; break; }
					}
					// monkey_dust: parentTypeIndex feeds m_types[] lookups in
					// countMembers/parseStructMembers/structMemberByIndex --
					// same out-of-bounds risk as classIndex elsewhere. The new
					// type occupies index m_types.size() once committed, so a
					// parent must reference an EARLIER (or root=0) index.
					if (sane && (info.parentTypeIndex < 0 || (size_t)info.parentTypeIndex > m_types.size())) {
						sane = false;
					}
					if (sane) {
						for (auto &m : info.members) {
							if (m.name.size() > 128) { sane = false; break; }
							for (unsigned char c : m.name) {
								if (c < 0x20 || c > 0x7E) { sane = false; break; }
							}
							if (!sane) break;
						}
					}
				} catch (...) {
					sane = false;
				}

				if (!sane) {
					m_stringPool = std::move(savedStringPool);
					if (tryResync((size_t)offsetBefore)) break;
					std::stringstream error;
					error << "Unsupported tag type " << type;
					throw std::runtime_error(error.str());
				}

				const auto &result = m_types.emplace_back(std::move(info));
				m_typeLookup.emplace(result.name, m_types.size() - 1);
				break;
			}

			case TagObjectRemember:
			{
				// monkey_dust: this whole case now runs through the same
				// snapshot/trial/rollback discipline as tryResync's own
				// TagObjectRemember branch. Reason: TagObjectRemember reached
				// via the NORMAL dispatch loop (not via resync) can equally
				// well be a coincidental false-tag match on garbage bytes --
				// confirmed in practice once parseStruct's classIndex bounds
				// check (see there) started throwing instead of silently
				// reading out-of-bounds memory. Without this, that throw
				// just kills the whole file instead of triggering recovery.
				auto savedStringPool = m_stringPool;
				auto savedObjects     = m_objects;
				auto savedNextAlloc   = m_nextAllocatedObject;
				bool ok = true;
				std::shared_ptr<HKXStruct> obj;
				try {
					auto it = m_objects.find(m_nextAllocatedObject);
					if (it == m_objects.end()) {
						HKX_TRACE_PRINTF("!!!!!!!!!! Creating new object %d\n", m_nextAllocatedObject);
						obj = std::make_shared<HKXStruct>();
						m_objects.emplace(m_nextAllocatedObject, obj);
						m_nextAllocatedObject++;
						parseStruct(*obj, 0);
					}
					else {
						HKX_TRACE_PRINTF("!!!!!!!!!! Reusing existing object %d\n", m_nextAllocatedObject);
						obj = it->second;
						m_nextAllocatedObject++;
						parseStruct(*obj, 0);
					}
					ok = obj && !obj->classNames.empty();
					if (ok) {
						for (auto &cn : obj->classNames) {
							for (unsigned char c : cn) if (c < 0x20 || c > 0x7E) { ok = false; break; }
							if (!ok) break;
						}
					}
				} catch (...) {
					ok = false;
				}

				if (!ok) {
					m_stringPool          = std::move(savedStringPool);
					m_objects             = std::move(savedObjects);
					m_nextAllocatedObject = savedNextAlloc;
					if (tryResync((size_t)offsetBefore)) break;
					std::stringstream error;
					error << "Unsupported tag type " << type;
					throw std::runtime_error(error.str());
				}

				break;
			}

			// monkey_dust: TAG_OBJECT/TAG_OBJECT_BACKREF/TAG_OBJECT_NULL were
			// completely missing from this switch (confirmed via the real
			// Havok SDK header, hkBinaryTagfileCommon.h -- these are documented,
			// real enum values, not a guess). TAG_OBJECT is "the following item
			// is an hkGenericObject" WITHOUT being remembered for backref --
			// unlike TagObjectRemember it must NOT consume an ID from
			// m_nextAllocatedObject (it was never assigned one).
			case TagObject:
			{
				// Same false-positive risk and same recovery discipline as
				// TagObjectRemember above (snapshot/trial/rollback).
				auto savedStringPool = m_stringPool;
				auto savedObjects     = m_objects;
				bool ok = true;
				HKXStruct tmp;
				try {
					HKX_TRACE_FPRINTF(stderr, "[trace] TagObject (unregistered, no backref)\n");
					parseStruct(tmp, 0);
					ok = !tmp.classNames.empty();
					if (ok) {
						for (auto &cn : tmp.classNames) {
							for (unsigned char c : cn) if (c < 0x20 || c > 0x7E) { ok = false; break; }
							if (!ok) break;
						}
					}
				} catch (...) {
					ok = false;
				}

				if (!ok) {
					m_stringPool = std::move(savedStringPool);
					m_objects    = std::move(savedObjects);
					if (tryResync((size_t)offsetBefore)) break;
					std::stringstream error;
					error << "Unsupported tag type " << type;
					throw std::runtime_error(error.str());
				}

				break;
			}

			// "Refer to a previously encountered object" -- reads an object ID
			// and doesn't need to do anything else (the object is already
			// fully parsed elsewhere); this tag is a top-level list-slot
			// marker, not a fresh parse.
			case TagObjectBackref:
			{
				auto objectIndex = m_stream.readVarInt();
				HKX_TRACE_FPRINTF(stderr, "[trace] TagObjectBackref -> %d\n", (int)objectIndex);
				// monkey_dust: a garbage objectIndex that doesn't refer to any
				// known object is another false-tag-match signature -- resync
				// instead of silently accepting a meaningless backref.
				if (m_objects.find(objectIndex) == m_objects.end()) {
					if (tryResync((size_t)offsetBefore)) break;
					std::stringstream error;
					error << "Unsupported tag type " << type;
					throw std::runtime_error(error.str());
				}
				break;
			}

			case TagObjectNull:
				HKX_TRACE_FPRINTF(stderr, "[trace] TagObjectNull\n");
				break;

			case TagFileEnd:
				goto breakOuter;

			default:
			{
				// monkey_dust: RECOVERY HEURISTIC -- see tryResync() for the
				// full rationale. Everything up to this point verified
				// byte-perfect against real decoded values (30-file batch
				// test, 1.8KB-380KB), but what follows hkaiNavMesh couldn't
				// be reverse-engineered from documentation (none found for
				// this exact Havok tagfile sub-version).
				if (tryResync((size_t)offsetBefore)) break;

				std::stringstream error;
				error << "Unsupported tag type " << type;
				throw std::runtime_error(error.str());
			}
			}
		}
	breakOuter:

		for (const auto &pair : m_objects) {
			if (pair.second->classNames.empty())
				// monkey_dust: was a hard throw. The RESYNC recovery path can
				// legitimately skip past a forward-referenced object's real
				// TagObjectRemember (e.g. objects 5/7 in Kenshi's navmeshes),
				// leaving an empty placeholder here -- warn instead of
				// aborting, since the objects that DID resolve (confirmed:
				// hkaiNavMesh with real vertices/faces/edges) are still valid.
				HKX_TRACE_FPRINTF(stderr, "[trace] WARNING: object %d never resolved (empty placeholder)\n", pair.first);
		}

		auto root = m_objects.find(1);
		if (root == m_objects.end()) {
			return {};
		}
		else {
			return root->second;
		}
	}

	bool HKXTagfileParser::tryResync(size_t startOff) {
		const unsigned char* base = m_mapping.data() + sizeof(TagfileHeader);
		size_t total = m_mapping.size() - sizeof(TagfileHeader);
		// monkey_dust: was capped at 8192 -- too small for complex/large
		// navmeshes, where the unrecovered "mystery" gap scales up along
		// with everything else. Scan the whole remaining file; each probe
		// is 1-2 cheap reads, so this stays fast even for the biggest tiles
		// (~380KB in the sample set).
		for (size_t tryOff = startOff + 1; tryOff < total; ++tryOff) {
			Deserializer probe(m_rules, base + tryOff, total - tryOff);
			int32_t tryTag;
			try { tryTag = probe.readVarInt(); } catch (...) { continue; }

			if (tryTag == TagFileEnd) {
				HKX_TRACE_FPRINTF(stderr, "[trace] RESYNC: TagFileEnd at offset %zu (skipped %zu bytes)\n",
				        tryOff, tryOff - startOff);
				m_stream = Deserializer(m_rules, base + tryOff, total - tryOff);
				return true;
			}
			if (tryTag == TagObjectRemember) {
				int32_t classIdx;
				try { classIdx = probe.readVarInt(); } catch (...) { continue; }
				if (classIdx <= 0 || (size_t)classIdx >= m_types.size()) continue;

				// monkey_dust: classIdx alone is too weak a check -- random
				// bytes have a real chance of landing in [1, m_types.size())
				// for files with many defined types, producing a false
				// positive that then corrupts m_types by reading garbage as
				// a "real" TagMetadata later (seen in practice: garbled
				// UTF-8 member names, eventual "out of bounds read"). Two
				// extra checks before committing:
				//   1. the resolved type's name must be a plausible
				//      identifier (non-empty, printable ASCII, sane length)
				//   2. the immediately-following member bitmap must not set
				//      any bit beyond the type's real member count -- a real
				//      encoder never writes presence bits for members that
				//      don't exist.
				const auto &typeInfo = m_types[classIdx];
				if (typeInfo.name.empty() || typeInfo.name.size() > 128) continue;
				bool nameOk = true;
				for (unsigned char c : typeInfo.name) {
					if (c < 0x20 || c > 0x7E) { nameOk = false; break; }
				}
				if (!nameOk) continue;

				size_t memberCount = 0;
				for (int32_t ti = classIdx; ti != 0; ti = m_types[ti].parentTypeIndex)
					memberCount += m_types[ti].members.size();
				size_t bitmapBytes = (memberCount + 7) / 8;
				if (bitmapBytes > 16) continue;  // matches MemberBitmap's own cap

				uint8_t bitmap[16] = {};
				try { probe.readBytes(bitmap, bitmapBytes); } catch (...) { continue; }
				bool bitmapOk = true;
				for (size_t bit = memberCount; bit < bitmapBytes * 8; ++bit) {
					if (bitmap[bit / 8] & (1 << (bit % 8))) { bitmapOk = false; break; }
				}
				if (!bitmapOk) continue;

				// monkey_dust: even with the checks above, a false-positive
				// classIndex+bitmap combo can still occur (confirmed in
				// practice: passed all header checks, then corrupted state
				// several TagMetadata reads later via a garbled member name
				// pulled from the string pool by coincidence). The only real
				// fix is to ACTUALLY trial-parse the candidate object and
				// verify the result is sane, with full rollback on failure
				// -- snapshot everything parseStruct can mutate
				// (m_stringPool grows via readString; m_objects/
				// m_nextAllocatedObject via nested TagTypeObject pointers).
				auto savedStringPool  = m_stringPool;
				auto savedObjects     = m_objects;
				auto savedNextAlloc   = m_nextAllocatedObject;
				Deserializer savedStream = std::move(m_stream);

				m_stream = Deserializer(m_rules, base + tryOff, total - tryOff);
				bool trialOk = false;
				try {
					auto existing = m_objects.find(m_nextAllocatedObject);
					std::shared_ptr<HKXStruct> obj;
					if (existing == m_objects.end()) {
						obj = std::make_shared<HKXStruct>();
						m_objects.emplace(m_nextAllocatedObject, obj);
					} else {
						obj = existing->second;
					}
					m_nextAllocatedObject++;
					parseStruct(*obj, 0);

					trialOk = !obj->classNames.empty();
					for (auto &cn : obj->classNames) {
						for (unsigned char c : cn) {
							if (c < 0x20 || c > 0x7E) { trialOk = false; break; }
						}
						if (!trialOk) break;
					}
				} catch (...) {
					trialOk = false;
				}

				if (trialOk) {
					HKX_TRACE_FPRINTF(stderr, "[trace] RESYNC: TagObjectRemember(classIndex=%d, %s) at offset %zu (skipped %zu bytes) -- trial-parsed OK\n",
					        classIdx, typeInfo.name.c_str(), tryOff, tryOff - startOff);
					// keep the mutated state and the advanced m_stream -- this
					// object is already fully consumed, the outer loop just
					// continues fresh from here.
					return true;
				}

				// Roll back everything and keep scanning.
				m_stringPool        = std::move(savedStringPool);
				m_objects           = std::move(savedObjects);
				m_nextAllocatedObject = savedNextAlloc;
				m_stream            = std::move(savedStream);
			}
		}
		return false;
	}

	size_t HKXTagfileParser::countMembers(int32_t classIndex) {
		size_t memberCount = 0;
		for (int32_t typeIndex = classIndex; typeIndex != 0; typeIndex = m_types[typeIndex].parentTypeIndex) {
			memberCount += m_types[typeIndex].members.size();
		}

		return memberCount;
	}

	void HKXTagfileParser::parseStruct(HKXStruct &st, int32_t classIndex) {
		// TagObjectRemember

		if (classIndex == 0) {
			classIndex = m_stream.readVarInt();
		}

		// monkey_dust: classIndex came straight from the stream with NO bounds
		// check (upstream trusted it blindly -- fine when every tag is
		// well-formed, but a coincidental garbage byte that decodes to a
		// plausible-looking tag (TagObject, or a resync candidate) can hand
		// this an out-of-range value). m_types[] is operator[] (no bounds
		// check) -- an out-of-range read here is undefined behaviour, not a
		// catchable exception, confirmed via gdb as a SIGSEGV inside HKX_TRACE_PRINTF()
		// itself (reading a garbage TagfileTypeInfo's corrupted std::string).
		// Converting it to a real, catchable exception is what lets
		// tryResync's try/catch actually do its job here.
		if (classIndex <= 0 || (size_t)classIndex >= m_types.size()) {
			throw std::runtime_error("classIndex out of range");
		}

		const auto &typeInfo = m_types[classIndex];

		auto memberCount = countMembers(classIndex);

		HKX_TRACE_PRINTF("Reading %s, total members: %zu\n", typeInfo.name.c_str(), memberCount);

		// monkey_dust: hkAabb special-case REVERTED -- offset math showed the
		// 32-byte forced read started 8 bytes before the real clean float
		// run, meaning aabb genuinely reads 0 bytes here (bitmap says both
		// min/max absent) and the actual problem is downstream.
		MemberBitmap memberBitmap;

		if (memberCount > memberBitmap.size() * 8)
			throw std::logic_error("too many members");

		m_stream.readBytes(memberBitmap.data(), (memberCount + 7) / 8);

		HKX_TRACE_PRINTF("Bitmap: ");
		for (size_t index = 0; index < (memberCount + 7) / 8; index++) {
			HKX_TRACE_PRINTF("%02X ", memberBitmap[index]);
		}
		HKX_TRACE_PRINTF("\n");

		size_t firstIndex = 0;

		parseStructMembers(st, memberBitmap, firstIndex, typeInfo);
	}

	void HKXTagfileParser::parseStructMembers(HKXStruct &st, const MemberBitmap &bitmap, size_t &firstIndex, const TagfileTypeInfo &typeInfo) {
		if (typeInfo.parentTypeIndex != 0) {
			parseStructMembers(st, bitmap, firstIndex, m_types[typeInfo.parentTypeIndex]);
		}

		HKX_TRACE_PRINTF("trying %s, first member: %zu, total members: %zu\n", typeInfo.name.c_str(), firstIndex, typeInfo.members.size());

		st.classNames.emplace_back(typeInfo.name);

		for (const auto &field : typeInfo.members) {
			size_t fieldIndex = firstIndex;

			auto off = m_stream.currentPtr() - (m_mapping.data() + sizeof(TagfileHeader));
			HKX_TRACE_PRINTF("index %zu: %s (offset=%ld)\n", fieldIndex, field.name.c_str(), (long)off);
			if (bitmap[fieldIndex / 8] & (1 << (fieldIndex % 8))) {
				HKX_TRACE_PRINTF("Field %s is present\n", field.name.c_str());

				parseField(st, field);

				auto offAfter = m_stream.currentPtr() - (m_mapping.data() + sizeof(TagfileHeader));
				HKX_TRACE_PRINTF("  (after %s: offset=%ld)\n", field.name.c_str(), (long)offAfter);
			}

			firstIndex++;
		}

		HKX_TRACE_PRINTF("finish with firstIndex %zu\n", firstIndex);

		//firstIndex += typeInfo.members.size();
	}

	const std::string &HKXTagfileParser::readString() {
		auto length = m_stream.readVarInt();

		if (length <= 0) {
			// monkey_dust: same class of bug as m_types[classIndex] --
			// operator[] with no bounds check on a value that came straight
			// from the stream. A garbage/corrupted "length" here (confirmed
			// in practice, reached via a false-positive TagMetadata match)
			// produced an out-of-range index -> UB -> SIGSEGV. Converting to
			// a real, catchable exception is what lets tryResync's try/catch
			// actually recover instead of crashing the whole process.
			size_t idx = (size_t)(-length);
			if (idx >= m_stringPool.size()) {
				throw std::runtime_error("string pool index out of range");
			}
			return m_stringPool[idx];
		}
		else {
			std::string newString;
			newString.resize(length);
			m_stream.readBytes(reinterpret_cast<unsigned char *>(newString.data()), newString.size());
			
			return m_stringPool.emplace_back(std::move(newString));
		}
	}

	TagfileTypeInfo HKXTagfileParser::readTypeInfo() {
		TagfileTypeInfo info;

		info.name = readString();
		info.unk3 = m_stream.readVarInt();
		info.parentTypeIndex = m_stream.readVarInt();

		auto memberCount = m_stream.readVarInt();
		HKX_TRACE_FPRINTF(stderr, "[trace] type name=%s unk3=%d parent=%d memberCount=%d\n",
		        info.name.c_str(), (int)info.unk3, (int)info.parentTypeIndex, (int)memberCount);
		info.members.resize(memberCount);

		for (auto &member : info.members) {
			member.name = readString();
			member.type = m_stream.readVarInt();
			HKX_TRACE_FPRINTF(stderr, "[trace]   member name=%s type=%d\n", member.name.c_str(), (int)member.type);

			if (member.type & TagTupleFlag) {
				member.tupleSize = m_stream.readVarInt();
			}

			if ((member.type & TagBasicTypeMask) == TagTypeObject || (member.type & TagBasicTypeMask) == TagTypeStruct) {
				member.className = readString();
			}
		}

		return info;
	}

	void HKXTagfileParser::parseField(HKXStruct &st, const TagfileMemberInfo &member) {
		if (member.type & ~(TagArrayFlag | TagTupleFlag | TagBasicTypeMask)) {
			std::stringstream error;
			error << "Unsupported flags in field type: " << member.type;
			throw std::runtime_error(error.str());
		}

		if (member.type == (TagTupleFlag | TagTypeByte)) {
			// Special case: byte tuple

			std::vector<unsigned char> bytes(member.tupleSize);
			m_stream.readBytes(bytes.data(), bytes.size());
			st.fields.emplace(member.name, std::move(bytes));
		}
		else if (member.type == (TagArrayFlag | TagTypeByte)) {
			// Special case: byte array

			auto byteLen = m_stream.readVarInt();
			std::vector<unsigned char> bytes(byteLen < 0 ? 0 : byteLen);
			m_stream.readBytes(bytes.data(), bytes.size());
			st.fields.emplace(member.name, std::move(bytes));
		} else if (member.type & (TagTupleFlag | TagArrayFlag)) {
			if ((member.type & (TagArrayFlag | TagTupleFlag)) == (TagArrayFlag | TagTupleFlag)) {
				throw std::logic_error("member is both an array and a tuple");
			}

			auto result = st.fields.emplace(member.name, HKXArray());
			auto &ary = std::get<HKXArray>(result.first->second);

			if (member.type & TagTupleFlag) {
				ary.values.resize(member.tupleSize);
			}
			else {
				auto len = m_stream.readVarInt();
				HKX_TRACE_FPRINTF(stderr, "[trace] array field %s: length=%d\n", member.name.c_str(), (int)len);
				// monkey_dust: found via batch-testing 30 tiles -- large/complex
				// navmeshes hit a NEGATIVE array length here (e.g. edgeData:
				// length=-1), which as size_t underflows to a huge value and
				// crashes vector::resize (std::length_error). Given -1 is
				// already a legitimate "no reference" sentinel elsewhere in
				// this format (oppositeEdge/oppositeFace/startUserEdgeIndex),
				// treating a negative array length as "empty array" (0) is the
				// most consistent interpretation, not a confirmed spec fact.
				ary.values.resize(len < 0 ? 0 : len);
			}

			parseArray(member, ary);

		}
		else {
			auto result = st.fields.emplace(member.name, std::monostate());			
			parseFieldValue(member.type & TagBasicTypeMask, member.className, result.first->second, -1);
		}
	}

	void HKXTagfileParser::parseArray(const TagfileMemberInfo &member, HKXArray &ary) {
		auto prefix = parseArrayPrefix(member.type);

		if ((member.type & TagBasicTypeMask) == TagTypeStruct) {
			parseStructArray(member, ary);
		}
		else {
			for (auto &value : ary.values) {
				parseFieldValue(member.type & TagBasicTypeMask, member.className, value, prefix);
			}
		}
	}

	int32_t HKXTagfileParser::parseArrayPrefix(unsigned int type) {
		if ((type & TagBasicTypeMask) == TagTypeInt) {
			HKX_TRACE_PRINTF("int prefix\n");
			auto arrayItemWidth = m_stream.readVarInt();
			return arrayItemWidth;
		} else if ((type & TagBasicTypeMask) == TagTypeVec4) {
			HKX_TRACE_PRINTF("vec4 prefix\n");
			auto numberOfMembers = m_stream.readVarInt();
			return numberOfMembers;
		}
		else {
			return -1;
		}
	}

	void HKXTagfileParser::parseFieldValue(unsigned int type, const std::string &className, HKXVariant &value, int32_t arrayPrefix) {
		HKX_TRACE_PRINTF("type: %u, className: %s, array prefix: %d\n", type, className.c_str(), arrayPrefix);

		switch (type) {
		case TagTypeByte:
			value = static_cast<uint64_t>(m_stream.readByte());
			break;

		case TagTypeInt:
		{
			auto v = m_stream.readVarInt();
			HKX_TRACE_FPRINTF(stderr, "[trace]   int value=%d\n", (int)v);
			value = static_cast<uint64_t>(static_cast<int64_t>(v));
			break;
		}

		case TagTypeReal:
		{
			float val;
			m_stream >> val;
			value = val;
			break;
		}

		case TagTypeVec4:
		{
			if (arrayPrefix < 0) {
				arrayPrefix = 4;
			}
			else if (arrayPrefix < 1 || arrayPrefix > 4) {
				throw std::logic_error("unsupported vec4 length");
			}

			value = HKXVector4();
			auto &base = std::get<HKXVector4>(value);
			base.x = 0.0f;
			base.y = 0.0f;
			base.z = 0.0f;
			base.w = 0.0f;

			auto *ptr = &base.x;
			for (int32_t index = 0; index < arrayPrefix; index++) {
				m_stream >> *ptr;

				ptr++;
			}

			break;
		}

		case TagTypeVec12:
			value = HKXMatrix3();
			m_stream >> std::get<HKXMatrix3>(value);
			break;

		case TagTypeVec16:
			value = HKXMatrix4();
			m_stream >> std::get<HKXMatrix4>(value);
			break;

		case TagTypeObject:
		{
			auto objectIndex = m_stream.readVarInt();
			if (objectIndex == 0) {
				HKX_TRACE_PRINTF("nullref\n");

				value = HKXStructRef();
			}
			else {
				auto it = m_objects.find(objectIndex);
				if (it != m_objects.end()) {
					HKX_TRACE_PRINTF("backref to %d\n", objectIndex);
					value = it->second;
				}
				else {
					HKX_TRACE_PRINTF("fwdref to %d\n", objectIndex);
					auto obj = std::make_shared<HKXStruct>();
					m_objects.emplace(objectIndex, obj);

					value = obj;
				}
			}

			break;
		}

		case TagTypeStruct:
		{
			int32_t classIndex = 0;
			if (!className.empty()) {
				auto it = m_typeLookup.find(className);
				if (it == m_typeLookup.end()) {
					std::stringstream stream;
					stream << "Class " << className << " not found";
					throw std::logic_error(stream.str());
				}

				classIndex = it->second;
			}
			value = HKXStruct();
			parseStruct(std::get<HKXStruct>(value), classIndex);
			break;
		}

		case TagTypeCString:
			value = readString();
			break;

		default:
		{
			std::stringstream error;
			error << "Unsupported field type: " << type;
			throw std::runtime_error(error.str());
		}
		}
	}

	const TagfileMemberInfo *HKXTagfileParser::structMemberByIndex(const TagfileTypeInfo &typeInfo, size_t index, size_t *firstIndex) {
		size_t firstIndexStorage = 0;
		if (!firstIndex) {
			firstIndex = &firstIndexStorage;
		}

		if (typeInfo.parentTypeIndex != 0) {
			auto result = structMemberByIndex(m_types[typeInfo.parentTypeIndex], index, firstIndex);
			if (result)
				return result;
		}

		if (index - *firstIndex < typeInfo.members.size()) {
			return &typeInfo.members[index - *firstIndex];
		}
		else {
			*firstIndex += typeInfo.members.size();

			return nullptr;
		}
	}

	void HKXTagfileParser::parseStructArray(const TagfileMemberInfo &member, HKXArray &ary) {
		int32_t classIndex = 0;

		if (member.type == (TagArrayFlag | TagTypeStruct) && member.className.empty()) {
			classIndex = m_stream.readVarInt();
		}
		else if (!member.className.empty()) {
			auto it = m_typeLookup.find(member.className);
			if (it == m_typeLookup.end()) {
				throw std::logic_error("unable to find class");
			}
			classIndex = it->second;
		}

		if (classIndex == 0) {
			throw std::logic_error("class index unknown in struct array");
		}
		// monkey_dust: same out-of-bounds guard as parseStruct() -- see there
		// for why this matters (converts UB into a catchable exception).
		if ((size_t)classIndex >= m_types.size()) {
			throw std::runtime_error("classIndex out of range");
		}

		const auto &typeInfo = m_types[classIndex];

		auto memberCount = countMembers(classIndex);

		MemberBitmap memberBitmap;

		if (memberCount > memberBitmap.size() * 8)
			throw std::logic_error("too many members");

		m_stream.readBytes(memberBitmap.data(), (memberCount + 7) / 8);

		HKX_TRACE_PRINTF("Bitmap: ");
		for (size_t index = 0; index < (memberCount + 7) / 8; index++) {
			HKX_TRACE_PRINTF("%02X ", memberBitmap[index]);
		}
		HKX_TRACE_PRINTF("\n");

		for (auto &member : ary.values) {
			member = HKXStruct();
			auto &st = std::get<HKXStruct>(member);

			for (auto typeIndex = classIndex; typeIndex != 0; typeIndex = m_types[typeIndex].parentTypeIndex) {
				st.classNames.emplace_back(m_types[typeIndex].name);
			}
		}

		for (int32_t index = 0; index < static_cast<int32_t>(memberCount); index++) {
			if (memberBitmap[index / 8] & (1 << (index % 8))) {
				auto memberType = structMemberByIndex(typeInfo, index);

				HKXArray view;
				view.values.resize(ary.values.size());

				HKX_TRACE_PRINTF("parsing array for %s\n", memberType->name.c_str());

				parseArray(*memberType, view);

				for (size_t index = 0, size = ary.values.size(); index < size; index++) {
					std::get<HKXStruct>(ary.values[index]).fields.emplace(memberType->name, std::move(view.values[index]));
				}
			}
		}

		HKX_TRACE_PRINTF("FINISHED WITH STRUCT ARRAY\n");
	}
}
