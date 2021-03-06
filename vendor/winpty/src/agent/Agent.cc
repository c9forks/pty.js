// Copyright (c) 2011-2015 Ryan Prichard
//
// Permission is hereby granted, free of charge, to any person obtaining a copy
// of this software and associated documentation files (the "Software"), to
// deal in the Software without restriction, including without limitation the
// rights to use, copy, modify, merge, publish, distribute, sublicense, and/or
// sell copies of the Software, and to permit persons to whom the Software is
// furnished to do so, subject to the following conditions:
//
// The above copyright notice and this permission notice shall be included in
// all copies or substantial portions of the Software.
//
// THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
// IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
// FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
// AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
// LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
// FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS
// IN THE SOFTWARE.

#include "Agent.h"
#include "Win32Console.h"
#include "ConsoleInput.h"
#include "Terminal.h"
#include "NamedPipe.h"
#include "ConsoleFont.h"
#include "../shared/DebugClient.h"
#include "../shared/AgentMsg.h"
#include "../shared/Buffer.h"
#include "../shared/winpty_snprintf.h"
#include "../shared/WinptyAssert.h"
#include "../shared/StringUtil.h"
#include "../shared/WindowsVersion.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <windows.h>
#include <vector>
#include <string>
#include <utility>

const int SC_CONSOLE_MARK = 0xFFF2;
const int SC_CONSOLE_SELECT_ALL = 0xFFF5;

namespace {

static BOOL WINAPI consoleCtrlHandler(DWORD dwCtrlType)
{
    if (dwCtrlType == CTRL_C_EVENT) {
        // Do nothing and claim to have handled the event.
        return TRUE;
    }
    return FALSE;
}

static std::string wstringToUtf8String(const std::wstring &input)
{
    int mblen = WideCharToMultiByte(CP_UTF8, 0,
                                    input.c_str(), input.size() + 1,
                                    NULL, 0, NULL, NULL);
    if (mblen <= 0) {
        return std::string();
    }
    std::vector<char> tmp(mblen);
    int mblen2 = WideCharToMultiByte(CP_UTF8, 0,
                                     input.c_str(), input.size() + 1,
                                     tmp.data(), tmp.size(),
                                     NULL, NULL);
    ASSERT(mblen2 == mblen);
    return tmp.data();
}

template <typename T>
T constrained(T min, T val, T max) {
    ASSERT(min <= max);
    return std::min(std::max(min, val), max);
}

static void sendSysCommand(HWND hwnd, int command) {
    SendMessage(hwnd, WM_SYSCOMMAND, command, 0);
}

static void sendEscape(HWND hwnd) {
    SendMessage(hwnd, WM_CHAR, 27, 0x00010001);
}

// In versions of the Windows console before Windows 10, the SelectAll and
// Mark commands both run quickly, but Mark changes the cursor position read
// by GetConsoleScreenBufferInfo.  Therefore, use SelectAll to be less
// intrusive.
//
// Starting with the new Windows 10 console, the Mark command no longer moves
// the cursor, and SelectAll uses a lot of CPU time.  Therefore, use Mark.
//
// The Windows 10 legacy-mode console behaves the same way as previous console
// versions, so detect which syscommand to use by testing whether Mark changes
// the cursor position.
static bool detectWhetherMarkMovesCursor(Win32Console &console)
{
    const ConsoleScreenBufferInfo info = console.bufferInfo();
    console.resizeBuffer(Coord(
        std::max<int>(2, info.dwSize.X),
        std::max<int>(2, info.dwSize.Y)));
    console.moveWindow(SmallRect(0, 0, 2, 2));
    const Coord initialPosition(1, 1);
    console.setCursorPosition(initialPosition);
    sendSysCommand(console.hwnd(), SC_CONSOLE_MARK);
    bool ret = console.cursorPosition() != initialPosition;
    sendEscape(console.hwnd());
    return ret;
}

} // anonymous namespace

Agent::Agent(LPCWSTR controlPipeName,
             LPCWSTR dataPipeName,
             int initialCols,
             int initialRows) :
    m_useMark(false),
    m_closingDataSocket(false),
    m_terminal(NULL),
    m_childProcess(NULL),
    m_childExitCode(-1),
    m_syncCounter(0),
    m_directMode(false),
    m_ptySize(initialCols, initialRows)
{
    trace("Agent::Agent entered");

    m_bufferData.resize(BUFFER_LINE_COUNT);

    m_console = new Win32Console;
    setSmallFont(m_console->conout());
    m_useMark = !detectWhetherMarkMovesCursor(*m_console);
    trace("Using %s syscommand to freeze console",
        m_useMark ? "MARK" : "SELECT_ALL");
    m_console->moveWindow(SmallRect(0, 0, 1, 1));
    m_console->resizeBuffer(Coord(initialCols, BUFFER_LINE_COUNT));
    m_console->moveWindow(SmallRect(0, 0, initialCols, initialRows));
    m_console->setCursorPosition(Coord(0, 0));
    m_console->setTitle(m_currentTitle);

    // For the sake of the color translation heuristic, set the console color
    // to LtGray-on-Black.
    m_console->setTextAttribute(7);
    m_console->clearAllLines(m_console->bufferInfo());

    m_controlSocket = makeSocket(controlPipeName);
    m_dataSocket = makeSocket(dataPipeName);
    m_terminal = new Terminal(m_dataSocket);
    m_consoleInput = new ConsoleInput(this);

    resetConsoleTracking(Terminal::OmitClear, m_console->windowRect());

    // Setup Ctrl-C handling.  First restore default handling of Ctrl-C.  This
    // attribute is inherited by child processes.  Then register a custom
    // Ctrl-C handler that does nothing.  The handler will be called when the
    // agent calls GenerateConsoleCtrlEvent.
    SetConsoleCtrlHandler(NULL, FALSE);
    SetConsoleCtrlHandler(consoleCtrlHandler, TRUE);

    // Disable Quick Edit mode.  The user has little control over winpty's
    // console, and I think it's better to default it off for the sake of
    // programs that care about mouse input.
    DWORD mode = 0;
    if (!GetConsoleMode(m_console->conin(), &mode)) {
        trace("Agent startup: GetConsoleMode failed");
    } else {
        mode &= ~ENABLE_QUICK_EDIT_MODE;
        if (!SetConsoleMode(m_console->conin(), mode)) {
            trace("Agent startup: SetConsoleMode failed");
        }
    }

    updateMouseInputFlags(true);
    setPollInterval(25);
}

Agent::~Agent()
{
    trace("Agent exiting...");
    agentShutdown();
    if (m_childProcess != NULL) {
        CloseHandle(m_childProcess);
    }
    delete m_console;
    delete m_terminal;
    delete m_consoleInput;
}

// Write a "Device Status Report" command to the terminal.  The terminal will
// reply with a row+col escape sequence.  Presumably, the DSR reply will not
// split a keypress escape sequence, so it should be safe to assume that the
// bytes before it are complete keypresses.
void Agent::sendDsr()
{
    m_dataSocket->write("\x1B[6n");
}

NamedPipe *Agent::makeSocket(LPCWSTR pipeName)
{
    NamedPipe *pipe = createNamedPipe();
    if (!pipe->connectToServer(pipeName)) {
        trace("error: could not connect to %s",
            utf8FromWide(pipeName).c_str());
        ::exit(1);
    }
    pipe->setReadBufferSize(64 * 1024);
    return pipe;
}

void Agent::resetConsoleTracking(
    Terminal::SendClearFlag sendClear, const SmallRect &windowRect)
{
    for (std::vector<ConsoleLine>::iterator
            it = m_bufferData.begin(), itEnd = m_bufferData.end();
            it != itEnd;
            ++it) {
        it->reset();
    }
    m_syncRow = -1;
    m_scrapedLineCount = windowRect.top();
    m_scrolledCount = 0;
    m_maxBufferedLine = -1;
    m_dirtyWindowTop = -1;
    m_dirtyLineCount = 0;
    m_terminal->reset(sendClear, m_scrapedLineCount);
}

void Agent::onPipeIo(NamedPipe *namedPipe)
{
    if (namedPipe == m_controlSocket)
        pollControlSocket();
    else if (namedPipe == m_dataSocket)
        pollDataSocket();
}

void Agent::pollControlSocket()
{
    if (m_controlSocket->isClosed()) {
        trace("Agent shutting down");
        shutdown();
        return;
    }

    while (true) {
        uint64_t packetSize = 0;
        const auto amt1 =
            m_controlSocket->peek(&packetSize, sizeof(packetSize));
        if (amt1 < sizeof(packetSize)) {
            break;
        }
        ASSERT(packetSize >= sizeof(packetSize) && packetSize <= SIZE_MAX);
        if (m_controlSocket->bytesAvailable() < packetSize) {
            if (m_controlSocket->readBufferSize() < packetSize) {
                m_controlSocket->setReadBufferSize(packetSize);
            }
            break;
        }
        std::vector<char> packetData;
        packetData.resize(packetSize);
        const auto amt2 = m_controlSocket->read(packetData.data(), packetSize);
        ASSERT(amt2 == packetSize);
        try {
            ReadBuffer buffer(std::move(packetData));
            buffer.getRawValue<uint64_t>(); // Discard the size.
            handlePacket(buffer);
        } catch (const ReadBuffer::DecodeError &error) {
            ASSERT(false && "Decode error");
        }
    }
}

void Agent::handlePacket(ReadBuffer &packet)
{
    int type = packet.getInt32();
    int32_t result = -1;
    switch (type) {
    case AgentMsg::Ping:
        result = 0;
        break;
    case AgentMsg::StartProcess:
        result = handleStartProcessPacket(packet);
        break;
    case AgentMsg::SetSize:
        // TODO: I think it might make sense to collapse consecutive SetSize
        // messages.  i.e. The terminal process can probably generate SetSize
        // messages faster than they can be processed, and some GUIs might
        // generate a flood of them, so if we can read multiple SetSize packets
        // at once, we can ignore the early ones.
        result = handleSetSizePacket(packet);
        break;
    case AgentMsg::GetExitCode:
        packet.assertEof();
        result = m_childExitCode;
        break;
    case AgentMsg::GetProcessId:
        packet.assertEof();
        if (m_childProcess == NULL)
            result = -1;
        else
            result = GetProcessId(m_childProcess);
        break;
    case AgentMsg::SetConsoleMode:
        m_terminal->setConsoleMode(packet.getInt32());
        result = 0;
        break;
    default:
        trace("Unrecognized message, id:%d", type);
    }
    m_controlSocket->write(&result, sizeof(result));
}

int Agent::handleStartProcessPacket(ReadBuffer &packet)
{
    ASSERT(m_childProcess == NULL);

    const auto program = packet.getWString();
    const auto cmdline = packet.getWString();
    const auto cwd = packet.getWString();
    const auto env = packet.getWString();
    const auto desktop = packet.getWString();
    packet.assertEof();

    auto cmdlineV = vectorWithNulFromString(cmdline);
    auto desktopV = vectorWithNulFromString(desktop);
    auto envV = vectorFromString(env);

    LPCWSTR programArg = program.empty() ? NULL : program.c_str();
    LPWSTR cmdlineArg = cmdline.empty() ? NULL : cmdlineV.data();
    LPCWSTR cwdArg = cwd.empty() ? NULL : cwd.c_str();
    LPWSTR envArg = env.empty() ? NULL : envV.data();

    STARTUPINFOW sui = {};
    PROCESS_INFORMATION pi = {};
    sui.cb = sizeof(sui);
    sui.lpDesktop = desktop.empty() ? NULL : desktopV.data();

    const BOOL success =
        CreateProcessW(programArg, cmdlineArg, NULL, NULL,
                       /*bInheritHandles=*/FALSE,
                       /*dwCreationFlags=*/CREATE_UNICODE_ENVIRONMENT |
                       /*CREATE_NEW_PROCESS_GROUP*/0,
                       envArg, cwdArg, &sui, &pi);
    const int ret = success ? 0 : GetLastError();

    trace("CreateProcess: %s %u",
          (success ? "success" : "fail"),
          static_cast<unsigned int>(pi.dwProcessId));

    if (success) {
        CloseHandle(pi.hThread);
        m_childProcess = pi.hProcess;
    }

    return ret;
}

int Agent::handleSetSizePacket(ReadBuffer &packet)
{
    int cols = packet.getInt32();
    int rows = packet.getInt32();
    packet.assertEof();
    resizeWindow(cols, rows);
    return 0;
}

void Agent::pollDataSocket()
{
    const std::string newData = m_dataSocket->readAllToString();
    if (hasDebugFlag("input_separated_bytes")) {
        // This debug flag is intended to help with testing incomplete escape
        // sequences and multibyte UTF-8 encodings.  (I wonder if the normal
        // code path ought to advance a state machine one byte at a time.)
        for (size_t i = 0; i < newData.size(); ++i) {
            m_consoleInput->writeInput(newData.substr(i, 1));
        }
    } else {
        m_consoleInput->writeInput(newData);
    }

    // If the child process had exited, then close the data socket if we've
    // finished sending all of the collected output.
    if (m_closingDataSocket &&
            !m_dataSocket->isClosed() &&
            m_dataSocket->bytesToSend() == 0) {
        trace("Closing data pipe after data is sent");
        m_dataSocket->closePipe();
    }
}

void Agent::updateMouseInputFlags(bool forceTrace)
{
    DWORD mode = 0;
    GetConsoleMode(m_console->conin(), &mode);
    const bool newFlagMI = mode & ENABLE_MOUSE_INPUT;
    const bool newFlagQE = mode & ENABLE_QUICK_EDIT_MODE;
    if (forceTrace ||
            newFlagMI != m_consoleMouseInputEnabled ||
            newFlagQE != m_consoleQuickEditEnabled) {
        trace("CONIN mode: ENABLE_MOUSE_INPUT=%s ENABLE_QUICK_EDIT_MODE=%s",
            newFlagMI ? "enabled" : "disabled",
            newFlagQE ? "enabled" : "disabled");
    }
    m_consoleMouseInputEnabled = newFlagMI;
    m_consoleQuickEditEnabled = newFlagQE;
    m_consoleInput->setMouseInputEnabled(newFlagMI && !newFlagQE);
}

void Agent::onPollTimeout()
{
    // Check the mouse input flag so we can output a trace message.
    updateMouseInputFlags();

    // Give the ConsoleInput object a chance to flush input from an incomplete
    // escape sequence (e.g. pressing ESC).
    m_consoleInput->flushIncompleteEscapeCode();

    // Check if the child process has exited.
    if (WaitForSingleObject(m_childProcess, 0) == WAIT_OBJECT_0) {
        DWORD exitCode;
        if (GetExitCodeProcess(m_childProcess, &exitCode))
            m_childExitCode = exitCode;
        CloseHandle(m_childProcess);
        m_childProcess = NULL;

        // Close the data socket to signal to the client that the child
        // process has exited.  If there's any data left to send, send it
        // before closing the socket.
        m_closingDataSocket = true;
    }

    // Scrape for output *after* the above exit-check to ensure that we collect
    // the child process's final output.
    if (!m_dataSocket->isClosed()) {
        syncConsoleContentAndSize(false);
    }

    if (m_closingDataSocket &&
            !m_dataSocket->isClosed() &&
            m_dataSocket->bytesToSend() == 0) {
        trace("Closing data pipe after child exit");
        m_dataSocket->closePipe();
    }
}

// Detect window movement.  If the window moves down (presumably as a
// result of scrolling), then assume that all screen buffer lines down to
// the bottom of the window are dirty.
void Agent::markEntireWindowDirty(const SmallRect &windowRect)
{
    m_dirtyLineCount = std::max(m_dirtyLineCount,
                                windowRect.top() + windowRect.height());
}

// Scan the screen buffer and advance the dirty line count when we find
// non-empty lines.
void Agent::scanForDirtyLines(const SmallRect &windowRect)
{
    const int w = m_readBuffer.rect().width();
    ASSERT(m_dirtyLineCount >= 1);
    const CHAR_INFO *const prevLine =
        m_readBuffer.lineData(m_dirtyLineCount - 1);
    WORD prevLineAttr = prevLine[w - 1].Attributes;
    const int stopLine = windowRect.top() + windowRect.height();

    for (int line = m_dirtyLineCount; line < stopLine; ++line) {
        const CHAR_INFO *lineData = m_readBuffer.lineData(line);
        for (int col = 0; col < w; ++col) {
            const WORD colAttr = lineData[col].Attributes;
            if (lineData[col].Char.UnicodeChar != L' ' ||
                    colAttr != prevLineAttr) {
                m_dirtyLineCount = line + 1;
                break;
            }
        }
        prevLineAttr = lineData[w - 1].Attributes;
    }
}

// Clear lines in the line buffer.  The `firstRow` parameter is in
// screen-buffer coordinates.
void Agent::clearBufferLines(
        const int firstRow,
        const int count,
        const WORD attributes)
{
    ASSERT(!m_directMode);
    for (int row = firstRow; row < firstRow + count; ++row) {
        const int64_t bufLine = row + m_scrolledCount;
        m_maxBufferedLine = std::max(m_maxBufferedLine, bufLine);
        m_bufferData[bufLine % BUFFER_LINE_COUNT].blank(attributes);
    }
}

// This function is called with the console frozen, and the console is still
// frozen when it returns.
void Agent::resizeImpl(const ConsoleScreenBufferInfo &origInfo)
{
    const int cols = m_ptySize.X;
    const int rows = m_ptySize.Y;

    {
        //
        // To accommodate Windows 10, erase all lines up to the top of the
        // visible window.  It's hard to tell whether this is strictly
        // necessary.  It ensures that the sync marker won't move downward,
        // and it ensures that we won't repeat lines that have already scrolled
        // up into the scrollback.
        //
        // It *is* possible for these blank lines to reappear in the visible
        // window (e.g. if the window is made taller), but because we blanked
        // the lines in the line buffer, we still don't output them again.
        //
        const Coord origBufferSize = origInfo.bufferSize();
        const SmallRect origWindowRect = origInfo.windowRect();

        if (!m_directMode) {
            m_console->clearLines(0, origWindowRect.Top, origInfo);
            clearBufferLines(0, origWindowRect.Top, origInfo.wAttributes);
            if (m_syncRow != -1) {
                createSyncMarker(m_syncRow);
            }
        }

        const Coord finalBufferSize(
            cols,
            // If there was previously no scrollback (e.g. a full-screen app
            // in direct mode) and we're reducing the window height, then
            // reduce the console buffer's height too.
            (origWindowRect.height() == origBufferSize.Y)
                ? rows
                : std::max<int>(rows, origBufferSize.Y));
        const bool cursorWasInWindow =
            origInfo.cursorPosition().Y >= origWindowRect.Top &&
            origInfo.cursorPosition().Y <= origWindowRect.Bottom;

        // Step 1: move the window.
        const int tmpWindowWidth = std::min(origBufferSize.X, finalBufferSize.X);
        const int tmpWindowHeight = std::min<int>(origBufferSize.Y, rows);
        SmallRect tmpWindowRect(
            0,
            std::min<int>(origBufferSize.Y - tmpWindowHeight,
                          origWindowRect.Top),
            tmpWindowWidth,
            tmpWindowHeight);
        if (cursorWasInWindow) {
            tmpWindowRect = tmpWindowRect.ensureLineIncluded(
                origInfo.cursorPosition().Y);
        }
        m_console->moveWindow(tmpWindowRect);

        // Step 2: resize the buffer.
        unfreezeConsole();
        m_console->resizeBuffer(finalBufferSize);
    }

    // Step 3: expand the window to its full size.
    {
        freezeConsole();
        const ConsoleScreenBufferInfo info = m_console->bufferInfo();
        const bool cursorWasInWindow =
            info.cursorPosition().Y >= info.windowRect().Top &&
            info.cursorPosition().Y <= info.windowRect().Bottom;

        SmallRect finalWindowRect(
            0,
            std::min<int>(info.bufferSize().Y - rows,
                          info.windowRect().Top),
            cols,
            rows);

        //
        // Once a line in the screen buffer is "dirty", it should stay visible
        // in the console window, so that we continue to update its content in
        // the terminal.  This code is particularly (only?) necessary on
        // Windows 10, where making the buffer wider can rewrap lines and move
        // the console window upward.
        //
        if (!m_directMode && m_dirtyLineCount > finalWindowRect.Bottom + 1) {
            // In theory, we avoid ensureLineIncluded, because, a massive
            // amount of output could have occurred while the console was
            // unfrozen, so that the *top* of the window is now below the
            // dirtiest tracked line.
            finalWindowRect = SmallRect(
                0, m_dirtyLineCount - rows,
                cols, rows);
        }

        // Highest priority constraint: ensure that the cursor remains visible.
        if (cursorWasInWindow) {
            finalWindowRect = finalWindowRect.ensureLineIncluded(
                info.cursorPosition().Y);
        }

        m_console->moveWindow(finalWindowRect);
        m_dirtyWindowTop = finalWindowRect.Top;
    }
}

void Agent::resizeWindow(const int cols, const int rows)
{
    if (cols < 1 ||
            cols > MAX_CONSOLE_WIDTH ||
            rows < 1 ||
            rows > BUFFER_LINE_COUNT - 1) {
        trace("resizeWindow: invalid size: cols=%d,rows=%d", cols, rows);
        return;
    }
    m_ptySize = Coord(cols, rows);
    syncConsoleContentAndSize(true);
}

void Agent::syncConsoleContentAndSize(bool forceResize)
{
    reopenConsole();
    freezeConsole();
    syncConsoleTitle();

    const ConsoleScreenBufferInfo info = m_console->bufferInfo();
    m_consoleInput->setMouseWindowRect(info.windowRect());

    // If an app resizes the buffer height, then we enter "direct mode", where
    // we stop trying to track incremental console changes.
    const bool newDirectMode = (info.bufferSize().Y != BUFFER_LINE_COUNT);
    if (newDirectMode != m_directMode) {
        trace("Entering %s mode", newDirectMode ? "direct" : "scrolling");
        resetConsoleTracking(Terminal::SendClear, info.windowRect());
        m_directMode = newDirectMode;

        // When we switch from direct->scrolling mode, make sure the console is
        // the right size.
        if (!m_directMode) {
            forceResize = true;
        }
    }

    if (m_directMode) {
        directScrapeOutput(info);
    } else {
        scrollingScrapeOutput(info);
    }

    if (forceResize) {
        resizeImpl(info);
    }

    unfreezeConsole();
}

void Agent::syncConsoleTitle()
{
    std::wstring newTitle = m_console->title();
    if (newTitle != m_currentTitle) {
        std::string command = std::string("\x1b]0;") +
                wstringToUtf8String(newTitle) + "\x07";
        m_dataSocket->write(command.c_str());
        m_currentTitle = newTitle;
    }
}

void Agent::directScrapeOutput(const ConsoleScreenBufferInfo &info)
{
    const Coord cursor = info.cursorPosition();
    const SmallRect windowRect = info.windowRect();

    const SmallRect scrapeRect(
        windowRect.left(), windowRect.top(),
        std::min<SHORT>(std::min(windowRect.width(), m_ptySize.X),
                        MAX_CONSOLE_WIDTH),
        std::min<SHORT>(std::min(windowRect.height(), m_ptySize.Y),
                        BUFFER_LINE_COUNT));
    const int w = scrapeRect.width();
    const int h = scrapeRect.height();

    largeConsoleRead(m_readBuffer, *m_console, scrapeRect);

    bool sawModifiedLine = false;
    for (int line = 0; line < h; ++line) {
        const CHAR_INFO *curLine =
            m_readBuffer.lineData(scrapeRect.top() + line);
        ConsoleLine &bufLine = m_bufferData[line];
        if (sawModifiedLine) {
            bufLine.setLine(curLine, w);
        } else {
            sawModifiedLine = bufLine.detectChangeAndSetLine(curLine, w);
        }
        if (sawModifiedLine) {
            //trace("sent line %d", line);
            m_terminal->sendLine(line, curLine, w);
        }
    }

    m_terminal->finishOutput(
        std::pair<int, int64_t>(
            constrained(0, cursor.X - scrapeRect.Left, w - 1),
            constrained(0, cursor.Y - scrapeRect.Top, h - 1)));
}

void Agent::scrollingScrapeOutput(const ConsoleScreenBufferInfo &info)
{
    const Coord cursor = info.cursorPosition();
    const SmallRect windowRect = info.windowRect();

    if (m_syncRow != -1) {
        // If a synchronizing marker was placed into the history, look for it
        // and adjust the scroll count.
        int markerRow = findSyncMarker();
        if (markerRow == -1) {
            // Something has happened.  Reset the terminal.
            trace("Sync marker has disappeared -- resetting the terminal"
                  " (m_syncCounter=%d)",
                  m_syncCounter);
            resetConsoleTracking(Terminal::SendClear, windowRect);
        } else if (markerRow != m_syncRow) {
            ASSERT(markerRow < m_syncRow);
            m_scrolledCount += (m_syncRow - markerRow);
            m_syncRow = markerRow;
            // If the buffer has scrolled, then the entire window is dirty.
            markEntireWindowDirty(windowRect);
        }
    }

    // Update the dirty line count:
    //  - If the window has moved, the entire window is dirty.
    //  - Everything up to the cursor is dirty.
    //  - All lines above the window are dirty.
    //  - Any non-blank lines are dirty.
    if (m_dirtyWindowTop != -1) {
        if (windowRect.top() > m_dirtyWindowTop) {
            // The window has moved down, presumably as a result of scrolling.
            markEntireWindowDirty(windowRect);
        } else if (windowRect.top() < m_dirtyWindowTop) {
            // The window has moved upward.  This is generally not expected to
            // happen, but the CMD/PowerShell CLS command will move the window
            // to the top as part of clearing everything else in the console.
            trace("Window moved upward -- resetting the terminal"
                  " (m_syncCounter=%d)",
                  m_syncCounter);
            resetConsoleTracking(Terminal::SendClear, windowRect);
        }
    }
    m_dirtyWindowTop = windowRect.top();
    m_dirtyLineCount = std::max(m_dirtyLineCount, cursor.Y + 1);
    m_dirtyLineCount = std::max(m_dirtyLineCount, (int)windowRect.top());

    // There will be at least one dirty line, because there is a cursor.
    ASSERT(m_dirtyLineCount >= 1);

    // The first line to scrape, in virtual line coordinates.
    const int64_t firstVirtLine = std::min(m_scrapedLineCount,
                                           windowRect.top() + m_scrolledCount);

    // Read all the data we will need from the console.  Start reading with the
    // first line to scrape, but adjust the the read area upward to account for
    // scanForDirtyLines' need to read the previous attribute.  Read to the
    // bottom of the window.  (It's not clear to me whether the
    // m_dirtyLineCount adjustment here is strictly necessary.  It isn't
    // necessary so long as the cursor is inside the current window.)
    const int firstReadLine = std::min<int>(firstVirtLine - m_scrolledCount,
                                            m_dirtyLineCount - 1);
    const int stopReadLine = std::max(windowRect.top() + windowRect.height(),
                                      m_dirtyLineCount);
    ASSERT(firstReadLine >= 0 && stopReadLine > firstReadLine);
    largeConsoleRead(m_readBuffer,
                     *m_console,
                     SmallRect(0, firstReadLine,
                               std::min<SHORT>(info.bufferSize().X,
                                               MAX_CONSOLE_WIDTH),
                               stopReadLine - firstReadLine));

    scanForDirtyLines(windowRect);

    // Note that it's possible for all the lines on the current window to
    // be non-dirty.

    // The line to stop scraping at, in virtual line coordinates.
    const int64_t stopVirtLine =
        std::min(m_dirtyLineCount, windowRect.top() + windowRect.height()) +
            m_scrolledCount;

    bool sawModifiedLine = false;

    const int w = m_readBuffer.rect().width();
    for (int64_t line = firstVirtLine; line < stopVirtLine; ++line) {
        const CHAR_INFO *curLine =
            m_readBuffer.lineData(line - m_scrolledCount);
        ConsoleLine &bufLine = m_bufferData[line % BUFFER_LINE_COUNT];
        if (line > m_maxBufferedLine) {
            m_maxBufferedLine = line;
            sawModifiedLine = true;
        }
        if (sawModifiedLine) {
            bufLine.setLine(curLine, w);
        } else {
            sawModifiedLine = bufLine.detectChangeAndSetLine(curLine, w);
        }
        if (sawModifiedLine) {
            //trace("sent line %d", line);
            m_terminal->sendLine(line, curLine, w);
        }
    }

    m_scrapedLineCount = windowRect.top() + m_scrolledCount;

    // Creating a new sync row requires clearing part of the console buffer, so
    // avoid doing it if there's already a sync row that's good enough.
    // TODO: replace hard-coded constants
    const int newSyncRow = static_cast<int>(windowRect.top()) - 200;
    if (newSyncRow >= 1 && newSyncRow >= m_syncRow + 200) {
        createSyncMarker(newSyncRow);
    }

    m_terminal->finishOutput(
        std::pair<int, int64_t>(cursor.X,
                                cursor.Y + m_scrolledCount));
}

void Agent::reopenConsole()
{
    // Reopen CONOUT.  The application may have changed the active screen
    // buffer.  (See https://github.com/rprichard/winpty/issues/34)
    delete m_console;
    m_console = new Win32Console();
}

void Agent::freezeConsole()
{
    sendSysCommand(m_console->hwnd(), m_useMark ? SC_CONSOLE_MARK
                                                : SC_CONSOLE_SELECT_ALL);
}

void Agent::unfreezeConsole()
{
    sendEscape(m_console->hwnd());
}

void Agent::syncMarkerText(CHAR_INFO (&output)[SYNC_MARKER_LEN])
{
    // XXX: The marker text generated here could easily collide with ordinary
    // console output.  Does it make sense to try to avoid the collision?
    char str[SYNC_MARKER_LEN + 1];
    winpty_snprintf(str, "S*Y*N*C*%08x", m_syncCounter);
    for (int i = 0; i < SYNC_MARKER_LEN; ++i) {
        output[i].Char.UnicodeChar = str[i];
        output[i].Attributes = 7;
    }
}

int Agent::findSyncMarker()
{
    ASSERT(m_syncRow >= 0);
    CHAR_INFO marker[SYNC_MARKER_LEN];
    CHAR_INFO column[BUFFER_LINE_COUNT];
    syncMarkerText(marker);
    SmallRect rect(0, 0, 1, m_syncRow + SYNC_MARKER_LEN);
    m_console->read(rect, column);
    int i;
    for (i = m_syncRow; i >= 0; --i) {
        int j;
        for (j = 0; j < SYNC_MARKER_LEN; ++j) {
            if (column[i + j].Char.UnicodeChar != marker[j].Char.UnicodeChar)
                break;
        }
        if (j == SYNC_MARKER_LEN)
            return i;
    }
    return -1;
}

void Agent::createSyncMarker(int row)
{
    ASSERT(row >= 1);

    // Clear the lines around the marker to ensure that Windows 10's rewrapping
    // does not affect the marker.
    m_console->clearLines(row - 1, SYNC_MARKER_LEN + 1,
                          m_console->bufferInfo());

    // Write a new marker.
    m_syncCounter++;
    CHAR_INFO marker[SYNC_MARKER_LEN];
    syncMarkerText(marker);
    m_syncRow = row;
    SmallRect markerRect(0, m_syncRow, 1, SYNC_MARKER_LEN);
    m_console->write(markerRect, marker);
}
