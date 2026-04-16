
#pragma once

#include <memory>
#include <string>
#include <vector>
#include <cstddef>
#include <cstdint>
#include <sstream>


#include "google/protobuf/util/message_differencer.h"


namespace VtxDiff::Protobuf
{
	static bool ProtoEquals(const google::protobuf::Message& A,const google::protobuf::Message& B,std::string* OutDiff)
	{
		using google::protobuf::util::MessageDifferencer;
		MessageDifferencer Diff;
		Diff.set_message_field_comparison(MessageDifferencer::EQUIVALENT);

		std::string report;
		if (OutDiff) Diff.ReportDifferencesToString(&report);

		const bool Ok = Diff.Compare(A, B);
		if (!Ok && OutDiff) *OutDiff = report;
		return Ok;
	}
}