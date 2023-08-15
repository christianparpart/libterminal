/**
 * This file is part of the "libterminal" project
 *   Copyright (c) 2019-2021 Christian Parpart <christian@parpart.family>
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#pragma once

#include <vtpty/Pty.h>

#include <crispy/BufferObject.h>

namespace terminal
{

class MockViewPty: public Pty
{
  public:
    explicit MockViewPty(PageSize windowSize): _pageSize { windowSize } {}

    void setReadData(std::string_view data);

    PtySlave& slave() noexcept override;
    [[nodiscard]] std::optional<std::tuple<std::string_view, bool>> read(crispy::buffer_object<char>& storage,
                                                                         std::chrono::milliseconds timeout,
                                                                         size_t size) override;
    void wakeupReader() override;
    int write(std::string_view data) override;
    [[nodiscard]] PageSize pageSize() const noexcept override;
    void resizeScreen(PageSize cells, std::optional<crispy::image_size> pixels = std::nullopt) override;

    void start() override;
    void close() override;
    [[nodiscard]] bool isClosed() const noexcept override;

    [[nodiscard]] std::string& stdinBuffer() noexcept { return _inputBuffer; }
    [[nodiscard]] std::string_view& stdoutBuffer() noexcept { return _outputBuffer; }

  private:
    PageSize _pageSize;
    std::optional<crispy::image_size> _pixelSize;
    std::string _inputBuffer;
    std::string_view _outputBuffer;
    bool _closed = false;
    PtySlaveDummy _slave;
};

} // namespace terminal
