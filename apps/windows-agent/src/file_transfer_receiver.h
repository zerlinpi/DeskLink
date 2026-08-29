#pragma once

#include <memory>

#include <rtc/rtc.hpp>

namespace desklink {

// Receives controller -> host files on the dedicated ordered/reliable
// `file-transfer` DataChannel. One file is active per channel; verified partial
// files are intentionally retained so a reconnect can resume by byte offset.
class FileTransferReceiver : public std::enable_shared_from_this<FileTransferReceiver> {
 public:
  static std::shared_ptr<FileTransferReceiver> Create(
      const std::shared_ptr<rtc::DataChannel>& channel);

  FileTransferReceiver(const FileTransferReceiver&) = delete;
  FileTransferReceiver& operator=(const FileTransferReceiver&) = delete;
  ~FileTransferReceiver();

  void Start();

 private:
  explicit FileTransferReceiver(std::shared_ptr<rtc::DataChannel> channel);
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

}  // namespace desklink
