/*
 * Vermont Aggregator Subsystem
 * Copyright (C) 2009 Vermont Project
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version 2
 * of the License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301, USA.
 *
 */

#include "PacketHashtable.h"
#include <iostream>
#include <fstream>

#include "common/crc.hpp"

#include "common/ipfixlolib/ipfix.h"
#include "common/Misc.h"
#include "common/Time.h"
#include "HashtableBuckets.h"


const uint32_t PacketHashtable::ExpHelperTable::UNUSED = 0xFFFFFFFF;

PacketHashtable::PacketHashtable(Source<IpfixRecord*>* recordsource, Rule* rule,
		uint16_t minBufferTime, uint16_t maxBufferTime, uint8_t hashbits)
	: BaseHashtable(recordsource, rule, minBufferTime, maxBufferTime, hashbits),
	snapshotWritten(false), startTime(time(0))
{
	buildExpHelperTable();
}


PacketHashtable::~PacketHashtable()
{
	delete[] expHelperTable.keyFields;
	delete[] expHelperTable.aggFields;
	delete[] expHelperTable.revAggFields;
	delete[] expHelperTable.varSrcPtrFields;
	delete[] expHelperTable.revKeyFieldMapper;
}

/**
 * copy functions which were extracted from ExpcopyData
 * those copy data from the original raw packet into the ipfix bucket in the hashtable
 * (always called, when a new bucket has to be created for a new flow)
 */
void PacketHashtable::copyDataEqualLengthNoMod(IpfixRecord::Data* bucket, const IpfixRecord::Data* src, ExpFieldData* efd)
{
	IpfixRecord::Data* dst = bucket+efd->dstIndex;
	memcpy(dst, src, efd->srcLength);
}
void PacketHashtable::copyDataGreaterLengthIPNoMod(IpfixRecord::Data* bucket, const IpfixRecord::Data* src, ExpFieldData* efd)
{
	IpfixRecord::Data* dst = bucket+efd->dstIndex;
	bzero(dst+efd->srcLength, efd->dstLength-efd->srcLength);
	memcpy(dst, src, efd->srcLength);
}
void PacketHashtable::copyDataGreaterLengthNoMod(IpfixRecord::Data* bucket, const IpfixRecord::Data* src, ExpFieldData* efd)
{
	IpfixRecord::Data* dst = bucket+efd->dstIndex;
	bzero(dst, efd->dstLength-efd->srcLength);
	memcpy(dst+efd->dstLength-efd->srcLength, src, efd->srcLength);
}
void PacketHashtable::copyDataSetOne(IpfixRecord::Data* bucket, const IpfixRecord::Data* src, ExpFieldData* efd)
{
	IpfixRecord::Data* dst = bucket+efd->dstIndex;
	memset(dst, 0, efd->dstLength);
	// set last byte of array to one (network byte order!)
	dst[efd->dstLength-1] = 1;
}
void PacketHashtable::copyDataSetZero(IpfixRecord::Data* bucket, const IpfixRecord::Data* src, ExpFieldData* efd)
{
	IpfixRecord::Data* dst = bucket+efd->dstIndex;
	memset(dst, 0, efd->dstLength);
}
void PacketHashtable::copyDataFrontPayload(IpfixRecord::Data* bucket, const IpfixRecord::Data* src, ExpFieldData* efd)
{
	aggregateFrontPayload(bucket, reinterpret_cast<const Packet*>(src), efd, true, false);
}
void PacketHashtable::copyDataFrontPayloadNoInit(IpfixRecord::Data* bucket, const IpfixRecord::Data* src, ExpFieldData* efd)
{
	aggregateFrontPayload(bucket, reinterpret_cast<const Packet*>(src), efd, true, true);
}
void PacketHashtable::copyDataDummy(IpfixRecord::Data* bucket, const IpfixRecord::Data* src, ExpFieldData* efd)
{
}
void PacketHashtable::copyDataMaxPacketGap(IpfixRecord::Data* bucket, const IpfixRecord::Data* src, ExpFieldData* efd)
{
	memset(bucket+efd->dstIndex, 0, efd->dstLength);
	memcpy(bucket+efd->privDataOffset, src, 8);
}
void PacketHashtable::copyDataNanoseconds(IpfixRecord::Data* bucket, const IpfixRecord::Data* src, ExpFieldData* efd)
{
	uint64_t ntptime;
	ntptime = ntp64timegcc(*reinterpret_cast<const struct timeval*>(src));
	uint64_t ntp2 = htonll(ntptime);
	DPRINTFL(MSG_VDEBUG, "ntp2: %llu, ntptime/ntp2 %llX/%llX", ntp2, ntptime, ntp2);
	memcpy(bucket+efd->dstIndex, &ntp2, sizeof(ntp2));
#ifdef DEBUG
	if (ntohll(*(uint64_t*)(bucket+efd->dstIndex))<(1000000000ULL+(2208988800ULL<<32)) || ntohll(*(uint64_t*)(bucket+efd->dstIndex))>(1300000000ULL+(2208988800ULL<<32))) {
		DPRINTFL(MSG_VDEBUG, "time before: %ds", reinterpret_cast<const struct timeval*>(src)->tv_sec);
		DPRINTFL(MSG_VDEBUG, "copy invalid end nano seconds: %lld s (%llX)", (ntohll(*(uint64_t*)(bucket+efd->dstIndex))>>32)-2208988800U, *(uint64_t*)(bucket+efd->dstIndex));
	}
#endif
}


/**
 * aggregates payload of packets to a certain maximum amount
 * only sequence number is regarded, succeeding packets with same sequence number
 * will overwrite data
 * ATTENTION: no stream reassembly is performed!
 */
void PacketHashtable::aggregateFrontPayload(IpfixRecord::Data* bucket, const Packet* src,
		const ExpFieldData* efd, bool firstpacket, bool onlyinit)
{
	DPRINTFL(MSG_VDEBUG, "called (%u, %hhu, %hhu)", efd->typeId, firstpacket, onlyinit);
	PayloadPrivateData* ppd = reinterpret_cast<PayloadPrivateData*>(bucket+efd->privDataOffset);

	if (onlyinit) {
		ASSERT(firstpacket, "firstpacket must be set to 1 when onlyinit==1!");
		ppd->initialized = 0;  // set data struct to "non-initialized"
		return;
	}

	IpfixRecord::Data* dst = bucket+efd->dstIndex;
	uint32_t seq = 0;
	if (src->ipProtocolType==Packet::TCP)
		seq = ntohl(*reinterpret_cast<const uint32_t*>(src->data+src->transportHeaderOffset+4));
	DPRINTFL(MSG_VDEBUG, "seq:%u, len:%u, udp:%u", seq, ppd->byteCount, src->ipProtocolType==Packet::UDP);

	if (firstpacket || !ppd->initialized) {
		if (src->ipProtocolType==Packet::TCP && src->data[src->transportHeaderOffset+13] & 0x02) {
			// SYN packet, so sequence number will be increased without any payload
			seq++;
		}
		// store sequence number and length of captured payload in private data
		ppd->seq = seq;
		ppd->byteCount = 0;
		ppd->initialized = 1;
	}

	// ignore packets that do either contain no payload or were not interpreted correctly
	if (src->payloadOffset==0 || src->payloadOffset==src->transportHeaderOffset) return;

	uint16_t plen = src->data_length-src->payloadOffset;

	if (plen>0) {
		uint32_t* pfplen = &ppd->byteCount;

		if (src->ipProtocolType==Packet::TCP) {
			uint32_t fseq = ppd->seq;
			uint32_t fpos = ppd->byteCount;

			DPRINTFL(MSG_VDEBUG, "plen:%u, fseq:%u, seq:%u, dstleng:%u", plen, fseq, seq, efd->dstLength);

			if (seq-fseq<efd->dstLength) {
				uint32_t pos = (seq!=0 ? seq-fseq : fpos);
				uint32_t len = efd->dstLength-pos;
				if (plen<len) len = plen;
				DPRINTFL(MSG_VDEBUG, "inserting payload data at %u with length %u", pos, len);
				memcpy(dst+pos, src->data+src->payloadOffset, len);
				uint32_t maxpos = pos+len;
				if (*pfplen<maxpos) *pfplen = maxpos;

				// increase packet counter (if available)
				if (efd->typeSpecData.frontPayload.pktCountOffset != ExpHelperTable::UNUSED)
					(*reinterpret_cast<uint32_t*>(bucket+efd->typeSpecData.frontPayload.pktCountOffset))++;
				// copy current length to corresponding inforamtion element
				if (efd->typeSpecData.frontPayload.fpaLenOffset != ExpHelperTable::UNUSED)
					(*reinterpret_cast<uint32_t*>(bucket+efd->typeSpecData.frontPayload.fpaLenOffset)) = *pfplen;
			}
		} else if (src->ipProtocolType==Packet::UDP){
			uint32_t* pfplen = &ppd->byteCount;
			if (*pfplen<efd->dstLength) {
				uint32_t len = efd->dstLength-*pfplen;
				if (plen<len) len = plen;
				DPRINTFL(MSG_VDEBUG, "inserting payload data at %u with length %u", *pfplen, len);
				memcpy(dst+(*pfplen), src->data+src->payloadOffset, len);
				*pfplen += len;

				// increase packet counter (if available)
				if (efd->typeSpecData.frontPayload.pktCountOffset != ExpHelperTable::UNUSED)
					(*reinterpret_cast<uint32_t*>(bucket+efd->typeSpecData.frontPayload.pktCountOffset))++;
				// copy current length to corresponding inforamtion element
				if (efd->typeSpecData.frontPayload.fpaLenOffset != ExpHelperTable::UNUSED)
					(*reinterpret_cast<uint32_t*>(bucket+efd->typeSpecData.frontPayload.fpaLenOffset)) = *pfplen;
			}
		}
	}

	DPRINTFL(MSG_VDEBUG, "new fplength: %u", *reinterpret_cast<uint32_t*>(bucket+efd->privDataOffset+4));
}


/**
 * helper function for buildExpHelperTable
 * does some error checking on given parameters and returns function which is appropriate
 * to copy field in flow
 */
void (*PacketHashtable::getCopyDataFunction(const ExpFieldData* efd))(IpfixRecord::Data*, const IpfixRecord::Data*, ExpFieldData*)
{
	// some error handling
	if (efd->modifier == Rule::Field::DISCARD) {
		THROWEXCEPTION("tried to copy data with field modifier set to discard");
	} else if ((efd->modifier != Rule::Field::KEEP) && (efd->modifier != Rule::Field::AGGREGATE) &&
			(efd->modifier < Rule::Field::MASK_START) && (efd->modifier > Rule::Field::MASK_END)) {
		THROWEXCEPTION("unknown modifier %d", efd->modifier);
	}
	switch (efd->typeId & ~IPFIX_REVERSE_TYPE) {
		case IPFIX_TYPEID_protocolIdentifier:
		case IPFIX_TYPEID_tcpControlBits:
			if (efd->dstLength != 1) {
				THROWEXCEPTION("unsupported length %d for type %d (\"%s\")", efd->dstLength, efd->typeId, typeid2string(efd->typeId));
			}
			break;

		case IPFIX_TYPEID_sourceTransportPort:
		case IPFIX_TYPEID_destinationTransportPort:
		case IPFIX_TYPEID_icmpTypeCode:
			if (efd->dstLength != 2) {
				THROWEXCEPTION("unsupported length %d for type %d (\"%s\")", efd->dstLength, efd->typeId, typeid2string(efd->typeId));
			}
			break;

		case IPFIX_TYPEID_flowStartSysUpTime:
		case IPFIX_TYPEID_flowStartSeconds:
		case IPFIX_TYPEID_flowEndSysUpTime:
		case IPFIX_TYPEID_flowEndSeconds:
		case IPFIX_ETYPEID_frontPayloadLen:
		case IPFIX_ETYPEID_frontPayloadPktCount:
		case IPFIX_ETYPEID_maxPacketGap:
			if (efd->dstLength != 4) {
				THROWEXCEPTION("unsupported length %d for type %d (\"%s\")", efd->dstLength, efd->typeId, typeid2string(efd->typeId));
			}
			break;

		case IPFIX_TYPEID_flowStartMilliSeconds:
		case IPFIX_TYPEID_flowStartMicroSeconds:
		case IPFIX_TYPEID_flowStartNanoSeconds:
		case IPFIX_TYPEID_flowEndMilliSeconds:
		case IPFIX_TYPEID_flowEndMicroSeconds:
		case IPFIX_TYPEID_flowEndNanoSeconds:
		case IPFIX_TYPEID_octetDeltaCount:
		case IPFIX_TYPEID_packetDeltaCount:
			if (efd->dstLength != 8) {
				THROWEXCEPTION("unsupported length %d for type %d (\"%s\")", efd->dstLength, efd->typeId, typeid2string(efd->typeId));
			}
			break;

		case IPFIX_TYPEID_sourceIPv4Address:
		case IPFIX_TYPEID_destinationIPv4Address:
			if (efd->dstLength != 5) {
				THROWEXCEPTION("unsupported length %d for type %d (\"%s\")", efd->dstLength, efd->typeId, typeid2string(efd->typeId));
			}
			break;
		case IPFIX_ETYPEID_frontPayload:
			if (efd->dstLength < 5) {
				THROWEXCEPTION("unsupported length %d for type %d (\"%s\")", efd->dstLength, efd->typeId, typeid2string(efd->typeId));
			}
			break;

		default:
			THROWEXCEPTION("type unhandled by Packet Aggregator: %d (\"%s\")", efd->typeId, typeid2string(efd->typeId));
			break;
	}

	// now decide on the correct copy function
	if (efd->typeId == IPFIX_ETYPEID_frontPayload) {
		return copyDataFrontPayload;
	} else if (efd->typeId == IPFIX_ETYPEID_revFrontPayload) {
		return copyDataFrontPayloadNoInit;
	} else if (efd->typeId == IPFIX_ETYPEID_frontPayloadLen) {
		return copyDataDummy;
	} else if (efd->typeId == IPFIX_ETYPEID_maxPacketGap) {
		return copyDataMaxPacketGap;
	} else if (efd->typeId == IPFIX_ETYPEID_frontPayloadPktCount ||
			   efd->typeId == IPFIX_ETYPEID_dpaFlowCount ||
			   efd->typeId == IPFIX_ETYPEID_dpaForcedExport) {
		return copyDataDummy;
	} else if (efd->typeId == IPFIX_TYPEID_flowStartNanoSeconds || efd->typeId == IPFIX_TYPEID_flowEndNanoSeconds) {
		return copyDataNanoseconds;
	} else if (efd->typeId & IPFIX_REVERSE_TYPE) {
		// ATTENTION: we treat all reverse elements the same: we set them to zero
		return copyDataSetZero;
	} else if (efd->dstLength == efd->srcLength) {
		return copyDataEqualLengthNoMod;
	} else if (efd->dstLength > efd->srcLength) {
		if (efd->typeId == IPFIX_TYPEID_sourceIPv4Address || efd->typeId == IPFIX_TYPEID_destinationIPv4Address) {
			if ((efd->modifier >= Rule::Field::MASK_START) && (efd->modifier <= Rule::Field::MASK_END)) {
				if (efd->dstLength != 5 && efd->srcLength != 5) {
					THROWEXCEPTION("destination and source data size must be 5, but is %d/%d - mask needs to be stored in both of them", efd->dstLength, efd->srcLength);
				}
				return copyDataEqualLengthNoMod;
			} else {
				return copyDataGreaterLengthIPNoMod;
			}
		} else if (efd->typeId == IPFIX_TYPEID_packetDeltaCount) {
			return copyDataSetOne;
		} else {
			return copyDataGreaterLengthNoMod;
		}
	} else {
		THROWEXCEPTION("target buffer too small. Expected buffer %s of length %d, got one with length %d", typeid2string(efd->typeId), efd->srcLength, efd->dstLength);
	}

	THROWEXCEPTION("this line should never be reached");
	return 0;
}

/**
 * @returns field length in bytes of corresponding entry in raw packet
 **/
uint8_t PacketHashtable::getRawPacketFieldLength(const InformationElement::IeInfo& type)
{
	if(type.enterprise != 0) {
		THROWEXCEPTION("don't know how to handle field id=%u, enterprise=%lu", type.id, type.enterprise);
	}

	if(type.enterprise == 0) {
		switch (type.id & ~IPFIX_REVERSE_TYPE) {
			case IPFIX_TYPEID_protocolIdentifier:
			case IPFIX_TYPEID_tcpControlBits:
			case IPFIX_TYPEID_packetDeltaCount:
			//case IPFIX_ETYPEID_revPacketDeltaCount:
				return 1;

			case IPFIX_TYPEID_icmpTypeCode:
			case IPFIX_TYPEID_sourceTransportPort:
			case IPFIX_TYPEID_destinationTransportPort:
			case IPFIX_TYPEID_octetDeltaCount:
			//case IPFIX_ETYPEID_revOctetDeltaCount:
				return 2;

			case IPFIX_TYPEID_flowStartSeconds:
			case IPFIX_TYPEID_flowEndSeconds:
			case IPFIX_TYPEID_sourceIPv4Address:
			case IPFIX_TYPEID_destinationIPv4Address:
			//case IPFIX_ETYPEID_revFlowStartSeconds:
			//case IPFIX_ETYPEID_revFlowEndSeconds:
			case IPFIX_ETYPEID_frontPayloadLen:
			case IPFIX_ETYPEID_maxPacketGap:
			case IPFIX_ETYPEID_frontPayloadPktCount:
				return 4;

			case IPFIX_TYPEID_flowStartMilliSeconds:
			case IPFIX_TYPEID_flowEndMilliSeconds:
			//case IPFIX_ETYPEID_revFlowStartMilliSeconds:
			//case IPFIX_ETYPEID_revFlowEndMilliSeconds:
			case IPFIX_TYPEID_flowStartNanoSeconds:
			case IPFIX_TYPEID_flowEndNanoSeconds:
			//case IPFIX_ETYPEID_revFlowStartNanoSeconds:
			//case IPFIX_ETYPEID_revFlowEndNanoSeconds:
				return 8;

			case IPFIX_ETYPEID_frontPayload:
				return type.length;				// length is variable and is set in configuration

			default:
				THROWEXCEPTION("PacketHashtable: unknown typeid, failed to determine raw packet field length");
				break;
		}
	}

	THROWEXCEPTION("PacketHashtable: unknown typeid, failed to determine raw packet field length");
	return 0;
}


/**
 * Returns offset to the given standard Information Element Id in the raw packet (relative to packet's netheader)
 * @param id standard Information Element Id
 * @param p pointer to raw packet
 * @returns offset (in bytes) at which the data for the given field is located in the raw packet
 */
uint16_t PacketHashtable::getRawPacketFieldOffset(InformationElement::IeId id, const Packet* p)
{
	switch (id & ~IPFIX_REVERSE_TYPE) {
		case IPFIX_TYPEID_packetDeltaCount:
			return 10;
			break;

		case IPFIX_TYPEID_flowStartSeconds:
		case IPFIX_TYPEID_flowEndSeconds:
			return reinterpret_cast<const unsigned char*>(&p->time_sec_nbo) - p->netHeader;
			break;

		case IPFIX_TYPEID_flowStartMilliSeconds:
		case IPFIX_TYPEID_flowEndMilliSeconds:
		case IPFIX_ETYPEID_maxPacketGap:
			return reinterpret_cast<const unsigned char*>(&p->time_msec_nbo) - p->netHeader;
			break;

		case IPFIX_TYPEID_flowStartNanoSeconds:
		case IPFIX_TYPEID_flowEndNanoSeconds:
			return reinterpret_cast<const unsigned char*>(&p->timestamp) - p->netHeader;
			break;

		case IPFIX_TYPEID_octetDeltaCount:
			return 2;
			break;

		case IPFIX_TYPEID_protocolIdentifier:
			return 9;
			break;

		case IPFIX_TYPEID_sourceIPv4Address:
			return 12;
			break;

		case IPFIX_TYPEID_destinationIPv4Address:
			return 16;
			break;

		case IPFIX_TYPEID_icmpTypeCode:
			if(p->ipProtocolType == Packet::ICMP) {
				return p->transportHeader + 0 - p->netHeader;
			} else {
				DPRINTFL(MSG_VDEBUG, "given id is %d, protocol is %d, but expected was %d or %d", id, p->ipProtocolType, Packet::UDP, Packet::TCP);

			}
			break;

		case IPFIX_TYPEID_sourceTransportPort:
			if((p->ipProtocolType == Packet::TCP) || (p->ipProtocolType == Packet::UDP)) {
				return p->transportHeader + 0 - p->netHeader;
			} else {
				DPRINTFL(MSG_VDEBUG, "given id is %d, protocol is %d, but expected was %d or %d", id, p->ipProtocolType, Packet::UDP, Packet::TCP);
			}
			break;

		case IPFIX_TYPEID_destinationTransportPort:
			if((p->ipProtocolType == Packet::TCP) || (p->ipProtocolType == Packet::UDP)) {
				return p->transportHeader + 2 - p->netHeader;
			} else {
				DPRINTFL(MSG_VDEBUG, "given id is %d, protocol is %d, but expected was %d or %d", id, p->ipProtocolType, Packet::UDP, Packet::TCP);
			}
			break;

		case IPFIX_TYPEID_tcpControlBits:
			if(p->ipProtocolType == Packet::TCP) {
				return p->transportHeader + 13 - p->netHeader;
			} else {
				DPRINTFL(MSG_VDEBUG, "given id is %d, protocol is %d, but expected was %d", id, p->ipProtocolType, Packet::TCP);
			}
			break;
	}

	// return just pointer to zero bytes as result
	return reinterpret_cast<const unsigned char*>(&p->zeroBytes) - p->netHeader;
}

/**
 * Returns offset to the given field data in the raw packet (relative to packet's netheader)
 * @param field Data Record field
 * @param p pointer to raw packet
 * @returns offset (in bytes) at which the data for the given field is located in the raw packet
 */
uint16_t PacketHashtable::getRawPacketFieldOffset(const InformationElement::IeInfo& type, const Packet* p)
{
	if(type.enterprise == 0) {
		return getRawPacketFieldOffset(type.id, p);
	}
	THROWEXCEPTION("don't know how to handle field id=%u, enterprise=%lu", type.id, type.enterprise);
	return 0; // should never be reached
}


/**
 * @returns if given field type is in varying positions inside a raw packet and inside the Packet structure
 */
bool PacketHashtable::isRawPacketPtrVariable(const InformationElement::IeInfo& type)
{
	switch (type.id & ~IPFIX_REVERSE_TYPE) {
		case IPFIX_TYPEID_packetDeltaCount:
		case IPFIX_TYPEID_flowStartSeconds:
		case IPFIX_TYPEID_flowEndSeconds:
		case IPFIX_TYPEID_flowStartMilliSeconds: // those elements are inside the Packet structure, not in the raw packet.
		case IPFIX_TYPEID_flowEndMilliSeconds:   // nevertheless, we may access it relative to the start of the packet data
		case IPFIX_TYPEID_flowStartNanoSeconds: //  ^
		case IPFIX_TYPEID_flowEndNanoSeconds:   //  ^
		case IPFIX_TYPEID_octetDeltaCount:
		case IPFIX_TYPEID_protocolIdentifier:
		case IPFIX_TYPEID_sourceIPv4Address:
		case IPFIX_TYPEID_destinationIPv4Address:
		case IPFIX_ETYPEID_maxPacketGap:
		case IPFIX_ETYPEID_frontPayloadPktCount:
			return false;

		case IPFIX_TYPEID_icmpTypeCode:
		case IPFIX_TYPEID_sourceTransportPort:
		case IPFIX_TYPEID_destinationTransportPort:
		case IPFIX_TYPEID_tcpControlBits:
		case IPFIX_ETYPEID_frontPayload:
		case IPFIX_ETYPEID_frontPayloadLen:
			return true;
	}

	THROWEXCEPTION("PacketHashtable: invalid type (%d), failed to determine variable state of field", type.id);
	return false;
}


/**
 * helper function for buildExpHelperTable
 */
void PacketHashtable::fillExpFieldData(ExpFieldData* efd, TemplateInfo::FieldInfo* hfi, Rule::Field::Modifier fieldModifier, uint16_t index)
{
	DPRINTFL(MSG_VDEBUG, "called for type id %u (%s)", hfi->type.id, typeid2string(hfi->type.id));
	efd->typeId = hfi->type.id;
	efd->dstIndex = hfi->offset;
	efd->dstLength = hfi->type.length;
	efd->srcLength = getRawPacketFieldLength(hfi->type);
	efd->modifier = fieldModifier;
	efd->varSrcIdx = isRawPacketPtrVariable(hfi->type);
	efd->privDataOffset = hfi->privDataOffset;

	// initialize static source index, if current field does not have a variable pointer
	if (!efd->varSrcIdx) {
		Packet p; // not good: create temporary packet just for initializing our optimization structure
		efd->srcIndex = getRawPacketFieldOffset(hfi->type.id, &p);
	}

	// special case for masked IPs: those contain variable pointers, if they are masked
	if ((efd->typeId==IPFIX_TYPEID_sourceIPv4Address || efd->typeId==IPFIX_TYPEID_destinationIPv4Address) &&
			(efd->modifier >= Rule::Field::MASK_START) && (efd->modifier <= Rule::Field::MASK_END)) {

		// ok, our dst/src ip has to be masked, so this is a variable pointer
		efd->varSrcIdx = true;
		// calculate inverse network mask using the modifier
		efd->data[4] = 32 - (efd->modifier - (int)Rule::Field::MASK_START);
		// save index of srcIndex, as this variable is overwritten by updatePointers for each packet
		efd->origSrcIndex = efd->srcIndex;

		switch (efd->typeId) {
			case IPFIX_TYPEID_sourceIPv4Address:
				expHelperTable.srcIpEFieldIndex = index;
				break;
			case IPFIX_TYPEID_destinationIPv4Address:
				expHelperTable.dstIpEFieldIndex = index;
				break;
		}

		// adjust srcLength, as our source length is 5 bytes including the appended mask!
		efd->srcLength = 5;
	}

	// set data efd field to the offset of IPFIX_ETYPEID_frontPayloadPktCount for front payload
	if (efd->typeId==IPFIX_ETYPEID_frontPayload) {
		*reinterpret_cast<uint32_t*>(efd->data) = 0xFFFFFFFF;
		for (int i=0; i<dataTemplate->fieldCount; i++) {
			TemplateInfo::FieldInfo* hfi = &dataTemplate->fieldInfo[i];
			if (hfi->type.id==IPFIX_ETYPEID_frontPayloadPktCount) {
				*reinterpret_cast<uint32_t*>(efd->data) = hfi->offset;
			}
		}
	}

	// mark field as variable, if needed
	if (efd->varSrcIdx)	{
		DPRINTF("marking type id %u as variable source pointer", efd->typeId);
		expHelperTable.varSrcPtrFields[expHelperTable.noVarSrcPtrFields++] = efd;
	}

	efd->copyDataFunc = getCopyDataFunction(efd);
}

/**
 * @returns if given field is available in a raw ip packet
 */
bool PacketHashtable::typeAvailable(const InformationElement::IeInfo& type)
{
	switch (type.id) {
		case IPFIX_TYPEID_packetDeltaCount:
		case IPFIX_TYPEID_flowStartSeconds:
		case IPFIX_TYPEID_flowEndSeconds:
		case IPFIX_TYPEID_flowStartMilliSeconds:
		case IPFIX_TYPEID_flowEndMilliSeconds:
		case IPFIX_TYPEID_flowStartNanoSeconds:
		case IPFIX_TYPEID_flowEndNanoSeconds:
		case IPFIX_TYPEID_octetDeltaCount:
		case IPFIX_TYPEID_protocolIdentifier:
		case IPFIX_TYPEID_sourceIPv4Address:
		case IPFIX_TYPEID_destinationIPv4Address:
		case IPFIX_TYPEID_icmpTypeCode:
		case IPFIX_TYPEID_sourceTransportPort:
		case IPFIX_TYPEID_destinationTransportPort:
		case IPFIX_TYPEID_tcpControlBits:
		case IPFIX_ETYPEID_frontPayloadLen:
		case IPFIX_ETYPEID_frontPayload:
		case IPFIX_ETYPEID_frontPayloadPktCount:
		case IPFIX_ETYPEID_maxPacketGap:
			return true;
	}

	return false;
}

/**
 * searches through all express structures and tries to find given information element
 * @returns offset of information element in destination template with start index at start of record data,
 *          returns ExpHelperTable::UNUSED if element was not found
 */
uint32_t PacketHashtable::getDstOffset(InformationElement::IeInfo ietype)
{
	uint32_t fcnt = 0;
	uint32_t offset = ExpHelperTable::UNUSED;
	for (uint32_t i=0; i<expHelperTable.noAggFields; i++) {
		ExpFieldData* efd = &expHelperTable.aggFields[i];
		if (efd->typeId==ietype.id) {
			fcnt++;
			offset = efd->dstIndex;
		}
	}
	for (uint32_t i=0; i<expHelperTable.noRevAggFields; i++) {
		ExpFieldData* efd = &expHelperTable.revAggFields[i];
		if (efd->typeId==ietype.id) {
			fcnt++;
			offset = efd->dstIndex;
		}
	}
	for (uint32_t i=0; i<expHelperTable.noKeyFields; i++) {
		ExpFieldData* efd = &expHelperTable.keyFields[i];
		if (efd->typeId==ietype.id) {
			fcnt++;
			offset = efd->dstIndex;
		}
	}
	if (fcnt>1) THROWEXCEPTION("Information element of type %s(%u) found more than once in template. This is not supported!", ietype.id, typeid2string(ietype.id));
	return offset;
}


/**
 * builds internal structure expHelperTable for fast aggregation of raw packets
 * used in the express aggregator
 */
void PacketHashtable::buildExpHelperTable()
{
	expHelperTable.keyFields = new ExpFieldData[dataTemplate->fieldCount];
	expHelperTable.aggFields = new ExpFieldData[dataTemplate->fieldCount];
	expHelperTable.revAggFields = new ExpFieldData[dataTemplate->fieldCount];
	expHelperTable.varSrcPtrFields = new ExpFieldData *[dataTemplate->fieldCount];
	expHelperTable.revKeyFieldMapper = new ExpFieldData *[dataTemplate->fieldCount];
	expHelperTable.noVarSrcPtrFields = 0;
	expHelperTable.useDPA = false;


	struct OffsetData {
		uint32_t* ehtPointer;
		InformationElement::IeId ieid;
		InformationElement::IeEnterpriseNumber ieen;
	};

	vector<uint16_t> expkey2field; // maps entries from expHelperTable to original fields in template
	vector<uint16_t> expagg2field;

	// TODO: fill with correct value!
	expHelperTable.dstIpEFieldIndex = false;
	expHelperTable.srcIpEFieldIndex = false;

	// at first, fill data structure with non-reversed aggregatable fields
	expHelperTable.noAggFields = 0;
	for (int i=0; i<dataTemplate->fieldCount; i++) {
		TemplateInfo::FieldInfo* hfi = &dataTemplate->fieldInfo[i];
		if (hfi->type.id & IPFIX_REVERSE_TYPE) continue;
		if (!typeAvailable(hfi->type)) {
			THROWEXCEPTION("Type '%s' is not contained in raw packet. Please remove it from PacketAggregator rule.", typeid2string(hfi->type.id));
		}
		if (!isToBeAggregated(hfi->type)) continue;
		ExpFieldData* efd = &expHelperTable.aggFields[expHelperTable.noAggFields++];
		fillExpFieldData(efd, hfi, fieldModifier[i], expHelperTable.noAggFields-1);
		expagg2field.push_back(i);
		if (hfi->type.id==IPFIX_ETYPEID_dpaForcedExport) {
			msg(MSG_INFO, "activated dialog-based payload aggregation");
			expHelperTable.useDPA = true;
		}
	}
	DPRINTF("got %u aggregated fields", expHelperTable.noAggFields);

	// now the key fields
	expHelperTable.noKeyFields = 0;
	for (int i=0; i<dataTemplate->fieldCount; i++) {
		TemplateInfo::FieldInfo* hfi = &dataTemplate->fieldInfo[i];
		if (isToBeAggregated(hfi->type)) continue;
		ExpFieldData* efd = &expHelperTable.keyFields[expHelperTable.noKeyFields++];
		fillExpFieldData(efd, hfi, fieldModifier[i], expHelperTable.noKeyFields-1);
		expkey2field.push_back(i);
	}
	DPRINTF("got %u key fields", expHelperTable.noKeyFields);

	// reversed aggregatable fields
	expHelperTable.noRevAggFields = 0;
	for (int i=0; i<dataTemplate->fieldCount; i++) {
		TemplateInfo::FieldInfo hfi = dataTemplate->fieldInfo[i];
		if ((hfi.type.id & IPFIX_REVERSE_TYPE) == 0) continue;
		hfi.type.id &= ~IPFIX_REVERSE_TYPE;
		if (!typeAvailable(hfi.type)) {
			THROWEXCEPTION("Type '%s' is not contained in raw packet. Please remove it from PacketAggregator rule.", typeid2string(hfi.type.id));
		}
		if (!isToBeAggregated(hfi.type)) continue;
		ExpFieldData* efd = &expHelperTable.revAggFields[expHelperTable.noRevAggFields++];
		fillExpFieldData(efd, &dataTemplate->fieldInfo[i], fieldModifier[i], expHelperTable.noRevAggFields-1);
	}
	DPRINTF("got %u reverse aggregated fields", expHelperTable.noRevAggFields);

	// build helper structure for mapping key fields to their reversed elements
	if (biflowAggregation) {
		for (uint32_t i=0; i<expHelperTable.noKeyFields; i++) {
			uint32_t fid = revKeyMapper[expkey2field[i]];
			vector<uint16_t>::iterator fit = find(expkey2field.begin(), expkey2field.end(), fid);
			if (fit==expkey2field.end()) THROWEXCEPTION("Error when calculating biflow table. This should not happen. (DOH!)");
			expHelperTable.revKeyFieldMapper[i] = &expHelperTable.keyFields[fit-expkey2field.begin()];
			DPRINTF("mapping key exph id %hu to id %hu", fit-expkey2field.begin(), i);
		}
	}
	DPRINTF("got %u fields with variable source pointers", expHelperTable.noVarSrcPtrFields);

	// search for offsets of fields that are linked with each other
	InformationElement::IeInfo ieinfo;
	ieinfo.enterprise = 0;
	for (uint32_t i=0; i<expHelperTable.noAggFields; i++) {
		ExpFieldData* efd = &expHelperTable.aggFields[i];
		switch (efd->typeId) {
			case IPFIX_ETYPEID_frontPayload:
				efd->typeSpecData.frontPayload.fpaLenOffset = getDstOffset({ IPFIX_ETYPEID_frontPayloadLen, 0, 0 });
				efd->typeSpecData.frontPayload.pktCountOffset = getDstOffset({ IPFIX_ETYPEID_frontPayloadPktCount, 0, 0 });
				efd->typeSpecData.frontPayload.dpaFlowCountOffset = getDstOffset({ IPFIX_ETYPEID_dpaFlowCount, 0, 0 });
				efd->typeSpecData.frontPayload.dpaForcedExportOffset = getDstOffset({ IPFIX_ETYPEID_dpaForcedExport, 0, 0 });
				efd->typeSpecData.frontPayload.dpaRevDataOffset = ExpHelperTable::UNUSED;
				if (expHelperTable.useDPA) {
					for (uint32_t i=0; i<expHelperTable.noAggFields; i++) {
						ExpFieldData* efd2 = &expHelperTable.aggFields[i];
						if (efd2->typeId==IPFIX_ETYPEID_dpaForcedExport) {
							efd->typeSpecData.frontPayload.dpaRevDataOffset = efd2->privDataOffset;
							break;
						}
					}
				}
				break;
		}
	}
	for (uint32_t i=0; i<expHelperTable.noRevAggFields; i++) {
		ExpFieldData* efd = &expHelperTable.revAggFields[i];
		switch (efd->typeId) {
			case IPFIX_ETYPEID_revFrontPayload:
				efd->typeSpecData.frontPayload.fpaLenOffset = getDstOffset({ IPFIX_ETYPEID_revFrontPayloadLen, 0, 0 });
				efd->typeSpecData.frontPayload.pktCountOffset = getDstOffset({ IPFIX_ETYPEID_revFrontPayloadPktCount, 0, 0 });
				efd->typeSpecData.frontPayload.dpaFlowCountOffset = getDstOffset({ IPFIX_ETYPEID_dpaFlowCount, 0, 0 });
				efd->typeSpecData.frontPayload.dpaForcedExportOffset = getDstOffset({ IPFIX_ETYPEID_dpaForcedExport, 0, 0 });
				break;
		}
	}
}


/**
 * calculates hash for given raw packet data in express aggregator
 */
uint32_t PacketHashtable::calculateHash(const IpfixRecord::Data* data)
{
	uint32_t hash = 0xAAAAAAAA;
	for (int i=0; i<expHelperTable.noKeyFields; i++) {
		ExpFieldData* efd = &expHelperTable.keyFields[i];
		DPRINTFL(MSG_VDEBUG, "hash for i=%u, typeid=%u, length=%u, srcpointer=%X", i, efd->typeId, efd->srcLength, reinterpret_cast<const char*>(data)+efd->srcIndex);
		hash = crc32(hash, efd->srcLength, reinterpret_cast<const char*>(data)+efd->srcIndex);
	}
	return hash & (htableSize-1);
}

/**
 * calculates hash for given raw packet data in express aggregator for reverse flows
 */
uint32_t PacketHashtable::calculateHashRev(const IpfixRecord::Data* data)
{
	uint32_t hash = 0xAAAAAAAA;
	for (int i=0; i<expHelperTable.noKeyFields; i++) {
		ExpFieldData* efd = expHelperTable.revKeyFieldMapper[i];
		DPRINTFL(MSG_VDEBUG, "hashrev for i=%u, typeid=%u, length=%u, srcpointer=%X", i, efd->typeId, efd->srcLength, reinterpret_cast<const char*>(data)+efd->srcIndex);
		hash = crc32(hash, efd->srcLength, reinterpret_cast<const char*>(data)+efd->srcIndex);
	}
	return hash & (htableSize-1);
}

/**
 * copies data from raw packet to a bucket which will be inserted into the hashtable
 * for aggregation (part of express aggregator)
 */
boost::shared_array<IpfixRecord::Data> PacketHashtable::buildBucketData(const Packet* p)
{
	// new field for insertion into hashtable
	boost::shared_array<IpfixRecord::Data> htdata(new IpfixRecord::Data[fieldLength+privDataLength]);
	IpfixRecord::Data* data = htdata.get();

	// copy all data ...
	for (int i=0; i<expHelperTable.noKeyFields; i++) {
		ExpFieldData* efd = &expHelperTable.keyFields[i];
		efd->copyDataFunc(data, reinterpret_cast<const uint8_t*>(p->netHeader)+efd->srcIndex, efd);
	}
	for (int i=0; i<expHelperTable.noAggFields; i++) {
		ExpFieldData* efd = &expHelperTable.aggFields[i];
		efd->copyDataFunc(data, reinterpret_cast<const uint8_t*>(p->netHeader)+efd->srcIndex, efd);
	}
	for (int i=0; i<expHelperTable.noRevAggFields; i++) {
		ExpFieldData* efd = &expHelperTable.revAggFields[i];
		efd->copyDataFunc(data, reinterpret_cast<const uint8_t*>(p->netHeader)+efd->srcIndex, efd);
	}

	return htdata;
}

/**
 * aggregates the given field of the raw packet data into a hashtable bucket
 * (part of express aggregator)
 */
void PacketHashtable::aggregateField(const ExpFieldData* efd, IpfixRecord::Data* bucket, const IpfixRecord::Data* deltaData)
{
	IpfixRecord::Data* baseData = bucket+efd->dstIndex;
	int64_t gap;

	uint64_t ntptime;
	uint64_t ntp2;

	switch (efd->typeId) {
		case IPFIX_TYPEID_flowStartSeconds:
			*(uint32_t*)baseData = lesserUint32Nbo(*(uint32_t*)baseData, *(uint32_t*)deltaData);
			break;

		case IPFIX_ETYPEID_revFlowStartSeconds:
			if (*(uint32_t*)baseData==0)
				*(uint32_t*)baseData = *(uint32_t*)deltaData;
			else
				*(uint32_t*)baseData = lesserUint32Nbo(*(uint32_t*)baseData, *(uint32_t*)deltaData);
			break;

		case IPFIX_TYPEID_flowStartMilliSeconds:
			*(uint64_t*)baseData = lesserUint64Nbo(*(uint64_t*)baseData, *(uint64_t*)deltaData);
			break;

		case IPFIX_ETYPEID_revFlowStartMilliSeconds:
			if (*(uint64_t*)baseData==0)
				*(uint64_t*)baseData = *(uint64_t*)deltaData;
			else
				*(uint64_t*)baseData = lesserUint64Nbo(*(uint64_t*)baseData, *(uint64_t*)deltaData);
			break;

		case IPFIX_TYPEID_flowStartNanoSeconds:
			ntptime = ntp64timegcc(*reinterpret_cast<const struct timeval*>(deltaData));
			ntp2 = htonll(ntptime);
			DPRINTFL(MSG_VDEBUG, "base: %lu s, delta: %lu s", (ntohll(*(uint64_t*)baseData)>>32)-2208988800U, ntohll(ntp2));
			DPRINTFL(MSG_VDEBUG, "base: %llX , delta: %llX", ntohll(*(uint64_t*)baseData), ntohll(ntp2));
			*(uint64_t*)baseData = lesserUint64Nbo(*(uint64_t*)baseData, ntp2);
#ifdef DEBUG
			if (ntohll(*(uint64_t*)baseData)<(1000000000ULL+(2208988800ULL<<32)) || ntohll(*(uint64_t*)baseData)>(1300000000ULL+(2208988800ULL<<32))) {
				DPRINTFL(MSG_VDEBUG, "invalid start nano seconds: %lu s", (ntohll(*(uint64_t*)baseData)>>32)-2208988800U);
				DPRINTFL(MSG_VDEBUG, "base: %llX , delta: %llX", *(uint64_t*)baseData, *(uint64_t*)deltaData);
			}
#endif
			break;

		case IPFIX_ETYPEID_revFlowStartNanoSeconds:
			if (*(uint64_t*)baseData==0)
				*(uint64_t*)baseData = *(uint64_t*)deltaData;
			else {
				ntptime = ntp64timegcc(*reinterpret_cast<const struct timeval*>(deltaData));
				ntp2 = htonll(ntptime);
				*(uint64_t*)baseData = lesserUint64Nbo(*(uint64_t*)baseData, ntp2);
			}
			break;

		case IPFIX_TYPEID_flowEndSeconds:
		case IPFIX_ETYPEID_revFlowEndSeconds:
			*(uint32_t*)baseData = greaterUint32Nbo(*(uint32_t*)baseData, *(uint32_t*)deltaData);
			break;

		case IPFIX_TYPEID_flowEndMilliSeconds:
		case IPFIX_ETYPEID_revFlowEndMilliSeconds:
			*(uint64_t*)baseData = greaterUint64Nbo(*(uint64_t*)baseData, *(uint64_t*)deltaData);
			break;

		case IPFIX_TYPEID_flowEndNanoSeconds:
		case IPFIX_ETYPEID_revFlowEndNanoSeconds:
			ntptime = ntp64timegcc(*reinterpret_cast<const struct timeval*>(deltaData));
			ntp2 = htonll(ntptime);
			*(uint64_t*)baseData = greaterUint64Nbo(*(uint64_t*)baseData, ntp2);
#ifdef DEBUG
			if (ntohll(*(uint64_t*)baseData)<(1000000000ULL+(2208988800ULL<<32)) || ntohll(*(uint64_t*)baseData)>(1300000000ULL+(2208988800ULL<<32)))
				DPRINTFL(MSG_VDEBUG, "invalid end nano seconds: %lu s", (ntohll(*(uint64_t*)baseData)>>32)-2208988800U);
#endif
			break;

		case IPFIX_TYPEID_octetDeltaCount: // 8 byte dst, 2 byte src
		case IPFIX_ETYPEID_revOctetDeltaCount:
			*(uint64_t*)baseData = htonll(ntohll(*(uint64_t*)baseData) + ntohs(*(uint16_t*)deltaData));
			break;

		case IPFIX_TYPEID_packetDeltaCount: // 8 byte dst, no src
		case IPFIX_ETYPEID_revPacketDeltaCount:
			*(uint64_t*)baseData = htonll(ntohll(*(uint64_t*)baseData)+1);
			break;

		case IPFIX_TYPEID_tcpControlBits:  // 1 byte src and dst, bitwise-or flows
		case IPFIX_ETYPEID_revTcpControlBits:
			*(uint8_t*)baseData |= *(uint8_t*)deltaData;
			break;

		case IPFIX_ETYPEID_frontPayload:
		case IPFIX_ETYPEID_revFrontPayload:
			aggregateFrontPayload(bucket, reinterpret_cast<const Packet*>(deltaData), efd, false, false);
			break;

		/*case IPFIX_ETYPEID_frontPayloadLen:
		case IPFIX_ETYPEID_revFrontPayloadLen:
			*(uint32_t*)baseData = htonl(reinterpret_cast<PayloadPrivateData*>(bucket+efd->privDataOffset)->byteCount);
			break;*/

		case IPFIX_ETYPEID_maxPacketGap:
		case IPFIX_ETYPEID_revMaxPacketGap:
			gap = (int64_t)ntohll(*(int64_t*)deltaData)-(int64_t)ntohll(*reinterpret_cast<const uint64_t*>(bucket+efd->privDataOffset));
			if (gap<0) gap = -gap;
			DPRINTFL(MSG_VDEBUG, "gap: %u, oldgap: %u", gap, ntohl(*(uint32_t*)baseData));

			if ((uint32_t)gap > ntohl(*(uint32_t*)baseData)) *(uint32_t*)baseData = htonl(gap);
			*reinterpret_cast<uint64_t*>(bucket+efd->privDataOffset) = *(uint64_t*)deltaData;
			break;

		case IPFIX_ETYPEID_frontPayloadLen:
		case IPFIX_ETYPEID_revFrontPayloadLen:
			// ignore these fields, as FPA aggregation does everything needed
			break;

			// no other types needed, as this is only for raw field input
		default:
			DPRINTF("non-aggregatable type: %d", efd->typeId);
			break;
	}
}

/**
 * aggregates the given raw packet data into the hashtable bucket
 */
void PacketHashtable::aggregateFlow(HashtableBucket* bucket, const Packet* p, bool reverse)
{
	IpfixRecord::Data* data = bucket->data.get();
	if (!reverse) {
		for (int i=0; i<expHelperTable.noAggFields; i++) {
			ExpFieldData* efd = &expHelperTable.aggFields[i];
			aggregateField(efd, data, p->netHeader+efd->srcIndex);
		}
	} else {
		for (int i=0; i<expHelperTable.noRevAggFields; i++) {
			ExpFieldData* efd = &expHelperTable.revAggFields[i];
			aggregateField(efd, data, p->netHeader+efd->srcIndex);
		}
	}
	// TODO: tobi_optimize
	// replace call of time() with access to a static variable which is updated regularly (such as every 100ms)
	bucket->expireTime = time(0) + minBufferTime;

	if (bucket->forceExpireTime>bucket->expireTime) {
		exportList.remove(bucket->listNode);
		exportList.push(bucket->listNode);
	}
}

/**
 * compares if given hashtable bucket data is equal with raw packet data
 * @returns true if equal, false if not equal
 */
bool PacketHashtable::equalFlow(IpfixRecord::Data* bucket, const Packet* p)
{
	for (int i=0; i<expHelperTable.noKeyFields; i++) {
		ExpFieldData* efd = &expHelperTable.keyFields[i];

		DPRINTFL(MSG_VDEBUG, "equal for i=%u, typeid=%u, length=%u, srcpointer=%X", i, efd->typeId, efd->srcLength, p->netHeader+efd->srcIndex);
		// just compare srcLength bytes, as we still have our original packet data
		if (memcmp(bucket+efd->dstIndex, p->netHeader+efd->srcIndex, efd->srcLength)!=0)
			return false;
	}
	return true;
}

/**
 * compares if given hashtable bucket data is equal with raw packet data, reverse version
 * (for biflow aggregation)
 * @returns true if equal, false if not equal
 */
bool PacketHashtable::equalFlowRev(IpfixRecord::Data* bucket, const Packet* p)
{
	for (int i=0; i<expHelperTable.noKeyFields; i++) {
		ExpFieldData* efdsrc = &expHelperTable.keyFields[i];
		ExpFieldData* efddst = expHelperTable.revKeyFieldMapper[i];

		DPRINTFL(MSG_VDEBUG, "equalrev for i=%u, typeid=%u, length=%u, srcpointer=%X", i, efdsrc->typeId, efdsrc->srcLength, p->netHeader+efdsrc->srcIndex);
		// just compare srcLength bytes, as we still have our original packet data
		if (memcmp(bucket+efddst->dstIndex, p->netHeader+efdsrc->srcIndex, efdsrc->srcLength)!=0)
			return false;
	}
	return true;
}


/**
 * masks ip addresses inside raw packet and creates a mask field
 * (part of express aggregator)
 */
void PacketHashtable::createMaskedField(IpfixRecord::Data* address, uint8_t imask)
{
	DPRINTF("unmasked address: %08X", *reinterpret_cast<uint32_t*>(address));
	if (imask > 0) {
		if (imask == 8) {
			address[3] = 0x00;
		} else if (imask == 16) {
			address[2] = 0x00;
			address[3] = 0x00;
		} else if (imask == 24) {
			address[1] = 0x00;
			address[2] = 0x00;
			address[3] = 0x00;
		} else if (imask == 32) {
			address[0] = 0x00;
			address[1] = 0x00;
			address[2] = 0x00;
			address[3] = 0x00;
		} else {
			// tobi_optimize: do this mask calculation during initialization phase of express aggregator
			int pattern = 0;
			int i;
			for(i = 0; i < imask; i++) {
				pattern |= (1 << i);
			}
			*(uint32_t*)address = htonl(ntohl(*(uint32_t*)(address)) & ~pattern);
		}
	}
	DPRINTF("masked address: %08X", *reinterpret_cast<uint32_t*>(address));
}


/**
 * masks ip addresses if desired in ExpFieldData->[0-3]
 * additional mask information (is 5th byte in aggregated data) is in ExpFieldData->data[4]
 */
void PacketHashtable::createMaskedFields(const Packet* p)
{
	if (expHelperTable.dstIpEFieldIndex > 0) {
		ExpFieldData* efd = &expHelperTable.keyFields[expHelperTable.dstIpEFieldIndex];
		// copy *original* ip address in *raw packet* to our temporary structure
		*reinterpret_cast<uint32_t*>(&efd->data[0]) = *reinterpret_cast<uint32_t*>(p->netHeader+efd->origSrcIndex);
		// then mask it
		createMaskedField(&efd->data[0], efd->data[4]);
	}
	if (expHelperTable.srcIpEFieldIndex > 0) {
		ExpFieldData* efd = &expHelperTable.keyFields[expHelperTable.srcIpEFieldIndex];
		// copy *original* ip address in *raw packet* to our temporary structure
		*reinterpret_cast<uint32_t*>(&efd->data[0]) = *reinterpret_cast<uint32_t*>(p->netHeader+efd->origSrcIndex);
		// then mask it
		createMaskedField(&efd->data[0], efd->data[4]);
	}
}

/**
 * updates variable pointers to the raw packet data for each packet
 * (part of express aggregator)
 */
void PacketHashtable::updatePointers(const Packet* p)
{
	for (int i=0; i<expHelperTable.noVarSrcPtrFields; i++) {
		ExpFieldData* efd = expHelperTable.varSrcPtrFields[i];

		switch (efd->typeId) {
			// perform a hack for masked IPs:
			// IP addresses which are to be masked are copied to efd->data[0-3] and masked there
			// now we need to do some pointer arithmetic to be able to access those transparently afterwards
			// note: only IP types to be masked have efd->varSrcIdx set
			case IPFIX_TYPEID_destinationIPv4Address:
			case IPFIX_TYPEID_sourceIPv4Address:
				efd->srcIndex = reinterpret_cast<uintptr_t>(&efd->data[0])-reinterpret_cast<uintptr_t>(p->netHeader);
				break;

			// aggregation and copy functions for frontPayload need to have source pointer
			// pointing to packet structure
			case IPFIX_ETYPEID_frontPayload:
			case IPFIX_ETYPEID_revFrontPayload:
				efd->srcIndex = reinterpret_cast<uintptr_t>(p)-reinterpret_cast<uintptr_t>(p->netHeader);
				break;

			default:
				// standard procedure for transport header fields
				efd->srcIndex = getRawPacketFieldOffset(efd->typeId, p);
				break;
		}
	}
}

void PacketHashtable::updateBucketData(HashtableBucket* bucket)
{
	statTotalEntries++;
	BucketListElement* node = hbucketIM.getNewInstance();
	node->reset();
	node->bucket = bucket;
	bucket->listNode = node;
	exportList.push(node);
}

/**
 * inserts the given raw packet into the hashtable
 * ATTENTION:
 *  - this function expects not to be called in parallel, as it uses internal buffers which are
 *    *NOT* thread-safe
 *  - hashes are calculated based on raw packet (masks are already applied then)
 */
void PacketHashtable::aggregatePacket(const Packet* p)
{
	// the following lock should almost never block (only during reconfiguration)
	while (atomic_lock(&aggInProgress)) {
		timespec req;
		req.tv_sec = 0;
		req.tv_nsec = 50000000;
		nanosleep(&req, &req);
	}


	DPRINTF("PacketHashtable::aggregatePacket()");
	updatePointers(p);
	createMaskedFields(p);

	uint32_t hash = calculateHash(p->netHeader);
	DPRINTFL(MSG_VDEBUG, "packet hash=%u", hash);

	// search bucket inside hashtable
	HashtableBucket* bucket = buckets[hash];
	HashtableBucket* appendbucket = NULL;
	bool flowfound = false;
	if (bucket != 0) {
		// This slot is already used, search spill chain for equal flow
		while (1) {
			if (equalFlow(bucket->data.get(), p)) {
				DPRINTF("aggregate flow in normal direction");
				aggregateFlow(bucket, p, 0);
				flowfound = true;
				break;
			}

			if (bucket->next==NULL) {
				appendbucket = bucket;
				break;
			}
			bucket = (HashtableBucket*)bucket->next;
		}
	}
	if (biflowAggregation && !flowfound) {
		// search for reverse direction
		uint32_t rhash = calculateHashRev(p->netHeader);
		DPRINTFL(MSG_VDEBUG, "rev packet hash=%u", rhash);
		HashtableBucket* bucket = buckets[rhash];

		while (bucket!=0) {
			if (equalFlowRev(bucket->data.get(), p)) {
				DPRINTF("aggregate flow in reverse direction");
				aggregateFlow(bucket, p, 1);
				flowfound = true;
				break;
			}
			bucket = (HashtableBucket*)bucket->next;
		}
	}

	if (!flowfound) {
		// create new flow
		if (appendbucket==NULL) {
			// create new bucket in empty hashtable entry
			statEmptyBuckets--;
			DPRINTF("creating new bucket");
			buckets[hash] = createBucket(buildBucketData(p), p->observationDomainID, 0, 0, hash);
			updateBucketData(buckets[hash]);
		} else {
			DPRINTF("creating bucket\n");
			statMultiEntries++;
			HashtableBucket* buck = createBucket(buildBucketData(p), p->observationDomainID, 0, appendbucket, hash);
			bucket->next = buck;
			updateBucketData(buck);
		}
	}
	//if (!snapshotWritten && (time(0)- 300 > starttime)) writeHashtable();
	// FIXME: enable snapshots again by configuration
	atomic_release(&aggInProgress);
}

void PacketHashtable::snapshotHashtable()
{
	// FIXME: this snapshotting code is not good ...
	int count = 0;
	ofstream fout("/home/sistmika/vermont/dos-attack/hashtable.txt");
	if (fout){

		fout << "bucket\tnumber\n";
		for(uint32_t i = 0; i < htableSize; i++){
			HashtableBucket* bucket = buckets[i];
			if (bucket == 0) count = 0;
			else{
				count++;
				while(bucket->next != 0){
					count++;
					bucket = (HashtableBucket*)bucket->next;
					}
			}
			fout << i+1 << "\t" << count  << "\n";
			count =0;
		}
		fout.close();
		snapshotWritten = true;
	}
	else {
	DPRINTF("unable to open file to write Hashtable\n");
	}
}

