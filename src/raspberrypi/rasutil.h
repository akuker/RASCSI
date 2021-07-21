//---------------------------------------------------------------------------
//
//	SCSI Target Emulator RaSCSI (*^..^*)
//	for Raspberry Pi
//
//	Powered by XM6 TypeG Technology.
//	Copyright (C) 2016-2020 GIMONS
//  Copyright (C) 2020 akuker
//
//---------------------------------------------------------------------------

#if !defined(rasutil_h)
#define rasutil_h

#include "google/protobuf/message_lite.h"

void SerializeMessage(int fd, const google::protobuf::MessageLite& message);
void DeserializeMessage(int fd, google::protobuf::MessageLite& message);

#endif
