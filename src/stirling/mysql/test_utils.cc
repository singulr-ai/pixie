#include "src/stirling/mysql/test_utils.h"

#include <math.h>
#include <deque>
#include <string>

#include "src/common/base/byte_utils.h"
#include "src/stirling/mysql/mysql.h"

namespace pl {
namespace stirling {
namespace mysql {
namespace testutils {

/**
 * These Gen functions help generate raw string or Packet needed for testing the MySQL parser
 * or stitcher, respectively. The caller are expected to use structured events in test_data.h
 * and revert them back to strings or packets as test input.
 */

/**
 * Generates raw MySQL packets in the form of a string.
 */
std::string GenRawPacket(uint8_t packet_num, std::string_view msg) {
  char header[4];
  utils::IntToLittleEndianByteStr(msg.size(), header);
  header[3] = packet_num;
  return absl::StrCat(CharArrayStringView(header), msg);
}

/**
 * Generate a raw MySQL packet from a Packet object.
 * @param packet The original Packet object.
 * @return the packet as a raw string, including header.
 */
std::string GenRawPacket(const Packet& packet) {
  return GenRawPacket(packet.sequence_id, packet.msg);
}

/**
 * Generates a raw packet with a string request.
 */
std::string GenRequestPacket(MySQLEventType command, std::string_view msg) {
  return GenRawPacket(0, absl::StrCat(CommandToString(command), msg));
}

/**
 * Generates the bytes of a length-encoded integer.
 * https://dev.mysql.com/doc/internals/en/integer.html#length-encoded-integer
 */
std::string GenLengthEncodedInt(int num) {
  DCHECK(num < pow(2, 64));
  if (num < 251) {
    char count_bytes[1];
    utils::IntToLittleEndianByteStr(num, count_bytes);
    return std::string(CharArrayStringView(count_bytes));
  } else if (num < pow(2, 16)) {
    char count_bytes[2];
    utils::IntToLittleEndianByteStr(num, count_bytes);
    return absl::StrCat("fc", CharArrayStringView(count_bytes));
  } else if (num < pow(2, 24)) {
    char count_bytes[3];
    utils::IntToLittleEndianByteStr(num, count_bytes);
    return absl::StrCat("fd", CharArrayStringView(count_bytes));
  } else {
    char count_bytes[8];
    utils::IntToLittleEndianByteStr(num, count_bytes);
    return absl::StrCat("fe", CharArrayStringView(count_bytes));
  }
}

/**
 * Generates the header packet of Resultset response. It contains num of cols.
 */
Packet GenCountPacket(uint8_t seq_id, int num_col) {
  Packet p;
  p.sequence_id = seq_id;
  p.msg = GenLengthEncodedInt(num_col);
  return p;
}

/**
 * Generates a Col Definition packet. Can be used in StmtPrepareResponse or Resultset.
 */
Packet GenColDefinition(uint8_t seq_id, const ColDefinition& col_def) {
  Packet p;
  p.sequence_id = seq_id;
  p.msg = col_def.msg;
  return p;
}

/**
 * Generates a resultset row.
 */
Packet GenResultsetRow(uint8_t seq_id, const ResultsetRow& row) {
  Packet p;
  p.sequence_id = seq_id;
  p.msg = row.msg;
  return p;
}

/**
 * Generates a header of StmtPrepare Response.
 */
Packet GenStmtPrepareRespHeader(uint8_t seq_id, const StmtPrepareRespHeader& header) {
  char statement_id[4];
  char num_columns[2];
  char num_params[2];
  char warning_count[2];
  utils::IntToLittleEndianByteStr(header.stmt_id, statement_id);
  utils::IntToLittleEndianByteStr(header.num_columns, num_columns);
  utils::IntToLittleEndianByteStr(header.num_params, num_params);
  utils::IntToLittleEndianByteStr(header.warning_count, warning_count);
  std::string msg = absl::StrCat(ConstStringView("\x00"), CharArrayStringView(statement_id),
                                 CharArrayStringView(num_columns), CharArrayStringView(num_params),
                                 ConstStringView("\x00"), CharArrayStringView(warning_count));

  Packet p;
  p.sequence_id = seq_id;
  p.msg = std::move(msg);
  return p;
}

/**
 * Generates a deque of packets. Contains a col counter packet and n resultset rows.
 */
std::deque<Packet> GenResultset(const Resultset& resultset, bool client_eof_deprecate) {
  uint8_t seq_id = 1;

  std::deque<Packet> result;
  result.emplace_back(GenCountPacket(seq_id++, resultset.num_col));
  for (const ColDefinition& col_def : resultset.col_defs) {
    result.emplace_back(GenColDefinition(seq_id++, col_def));
  }
  if (!client_eof_deprecate) {
    result.emplace_back(GenEOF(seq_id++));
  }
  for (const ResultsetRow& row : resultset.results) {
    result.emplace_back(GenResultsetRow(seq_id++, row));
  }
  if (client_eof_deprecate) {
    result.emplace_back(GenOK(seq_id++));
  } else {
    result.emplace_back(GenEOF(seq_id++));
  }
  return result;
}

/**
 * Generates a StmtPrepareOkResponse.
 */
std::deque<Packet> GenStmtPrepareOKResponse(const StmtPrepareOKResponse& resp) {
  uint8_t seq_id = 1;

  std::deque<Packet> result;
  result.push_back(GenStmtPrepareRespHeader(seq_id++, resp.header));

  for (const ColDefinition& param_def : resp.param_defs) {
    ColDefinition p{param_def.msg};
    result.push_back(GenColDefinition(seq_id++, p));
  }
  result.push_back(GenEOF(seq_id++));

  for (const ColDefinition& col_def : resp.col_defs) {
    ColDefinition c{col_def.msg};
    result.push_back(GenColDefinition(seq_id++, c));
  }
  result.push_back(GenEOF(seq_id++));
  return result;
}

Packet GenStmtExecuteRequest(const StmtExecuteRequest& req) {
  char statement_id[4];
  utils::IntToLittleEndianByteStr(req.stmt_id, statement_id);
  std::string msg =
      absl::StrCat(CommandToString(MySQLEventType::kStmtExecute), CharArrayStringView(statement_id),
                   ConstStringView("\x00\x01\x00\x00\x00"));
  int num_params = req.params.size();
  if (num_params > 0) {
    for (int i = 0; i < (num_params + 7) / 8; i++) {
      msg += ConstStringView("\x00");
    }
    msg += "\x01";
  }
  for (const ParamPacket& param : req.params) {
    switch (param.type) {
      // TODO(chengruizhe): Add more types.
      case MySQLColType::kString:
        msg += ConstStringView("\xfe\x00");
        break;
      default:
        msg += ConstStringView("\xfe\x00");
        break;
    }
  }
  for (const ParamPacket& param : req.params) {
    msg += GenLengthEncodedInt(param.value.size());
    msg += param.value;
  }
  Packet p;
  p.msg = std::move(msg);
  return p;
}

Packet GenStmtCloseRequest(const StmtCloseRequest& req) {
  char statement_id[4];
  utils::IntToLittleEndianByteStr(req.stmt_id, statement_id);
  std::string msg =
      absl::StrCat(CommandToString(MySQLEventType::kStmtClose), CharArrayStringView(statement_id));
  Packet p;
  p.msg = std::move(msg);
  return p;
}

/**
 * Generates a String Request packet of the specified type.
 */
Packet GenStringRequest(const StringRequest& req, MySQLEventType command) {
  DCHECK_LE(static_cast<uint8_t>(command), kMaxCommandValue);
  Packet p;
  p.msg = absl::StrCat(CommandToString(command), req.msg);
  return p;
}

/**
 * Generates a Err packet.
 */
Packet GenErr(uint8_t seq_id, const ErrResponse& err) {
  char error_code[2];
  utils::IntToLittleEndianByteStr(err.error_code, error_code);
  std::string msg = absl::StrCat("\xff", CharArrayStringView(error_code),
                                 "\x23\x48\x59\x30\x30\x30", err.error_message);
  Packet p;
  p.sequence_id = seq_id;
  p.msg = std::move(msg);
  return p;
}

/**
 * Generates a OK packet. Content is fixed.
 */
Packet GenOK(uint8_t seq_id) {
  std::string msg = ConstString("\x00\x00\x00\x02\x00\x00\x00");
  Packet p;
  p.sequence_id = seq_id;
  p.msg = std::move(msg);
  return p;
}

/**
 * Generates a EOF packet. Content is fixed.
 */
Packet GenEOF(uint8_t seq_id) {
  std::string msg = ConstString("\xfe\x00\x00\x22\x00");
  Packet p;
  p.sequence_id = seq_id;
  p.msg = std::move(msg);
  return p;
}

}  // namespace testutils
}  // namespace mysql
}  // namespace stirling
}  // namespace pl
