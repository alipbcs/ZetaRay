#include "Error.h"
#include "Span.h"
#include "../Win32/Win32.h"
#include "../Support/MemoryArena.h"

using namespace ZetaRay::Util;
using namespace ZetaRay::Support;

// Ref: https://gist.github.com/rioki/85ca8295d51a5e0b7c56e5005b0ba8b4
//
// Debug Helpers
// 
// Copyright (c) 2015 - 2017 Sean Farrell <sean.farrell@rioki.org>
// 
// Permission is hereby granted, free of charge, to any person obtaining a copy
// of this software and associated documentation files (the "Software"), to deal
// in the Software without restriction, including without limitation the rights
// to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
// copies of the Software, and to permit persons to whom the Software is
// furnished to do so, subject to the following conditions:
// 
// The above copyright notice and this permission notice shall be included in
// all copies or substantial portions of the Software.
// 
// THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
// IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
// FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
// AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
// LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
// OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
// THE SOFTWARE.
// 

#ifdef _DEBUG

#include <dbghelp.h>
#include <string>
#include "../App/Log.h"

#define DBG_TRACE(MSG, ...)  dbg::trace(MSG, __VA_ARGS__)

#define BUFF_SIZE 2048
#define MAX_NUM_STACK_FRAMES 16llu

namespace dbg
{
    //void trace(const char* msg, ...)
    //{
    //    char buff[1024];

    //    va_list args;
    //    va_start(args, msg);
    //    vsnprintf(buff, 1024, msg, args);

    //    OutputDebugStringA(buff);

    //    va_end(args);
    //}

    std::string basename(const std::string& file)
    {
        size_t i = file.find_last_of("\\/");
        if (i == std::string::npos)
            return file;
        else
            return file.substr(i + 1);
    }

    struct StackFrame
    {
        DWORD64 address;
        std::string name;
        std::string module;
        unsigned int line;
        std::string file;
    };

    void stack_trace(Vector<StackFrame, ArenaAllocator>& frames)
    {
        DWORD machine = IMAGE_FILE_MACHINE_AMD64;
        HANDLE process = GetCurrentProcess();
        HANDLE thread = GetCurrentThread();

        if (SymInitialize(process, NULL, TRUE) == FALSE)
        {
            LOG(__FUNCTION__ ": Failed to call SymInitialize.");
            return;
        }

        SymSetOptions(SYMOPT_LOAD_LINES);

        CONTEXT context = {};
        context.ContextFlags = CONTEXT_FULL;
        RtlCaptureContext(&context);

        STACKFRAME frame = {};
        frame.AddrPC.Offset = context.Rip;
        frame.AddrPC.Mode = AddrModeFlat;
        frame.AddrFrame.Offset = context.Rbp;
        frame.AddrFrame.Mode = AddrModeFlat;
        frame.AddrStack.Offset = context.Rsp;
        frame.AddrStack.Mode = AddrModeFlat;

        bool first = true;

        while (StackWalk(machine, process, thread, &frame, &context, NULL, SymFunctionTableAccess, SymGetModuleBase, NULL))
        {
            StackFrame f = {};
            f.address = frame.AddrPC.Offset;

            DWORD64 moduleBase = 0;

            moduleBase = SymGetModuleBase(process, frame.AddrPC.Offset);

            char moduelBuff[MAX_PATH];
            if (moduleBase && GetModuleFileNameA((HINSTANCE)moduleBase, moduelBuff, MAX_PATH))
                f.module = basename(moduelBuff);
            else
                f.module = "Unknown Module";

            DWORD64 offset = 0;

            char symbolBuffer[sizeof(IMAGEHLP_SYMBOL) + 255];
            PIMAGEHLP_SYMBOL symbol = (PIMAGEHLP_SYMBOL)symbolBuffer;
            symbol->SizeOfStruct = (sizeof IMAGEHLP_SYMBOL) + 255;
            symbol->MaxNameLength = 254;

            if (SymGetSymFromAddr(process, frame.AddrPC.Offset, &offset, symbol))
                f.name = symbol->Name;
            else
            {
                DWORD error = GetLastError();
                LOG(__FUNCTION__ ": Failed to resolve address 0x%llX: %u\n", frame.AddrPC.Offset, error);
                f.name = "Unknown Function";
            }

            IMAGEHLP_LINE line;
            line.SizeOfStruct = sizeof(IMAGEHLP_LINE);

            DWORD offset_ln = 0;
            if (SymGetLineFromAddr(process, frame.AddrPC.Offset, &offset_ln, &line))
            {
                f.file = line.FileName;
                f.line = line.LineNumber;
            }
            else
            {
                DWORD error = GetLastError();
                LOG(__FUNCTION__ ": Failed to resolve line for 0x%llX: %u\n", frame.AddrPC.Offset, error);
                f.line = 0;
            }

            if (!first)
                frames.push_back(f);

            first = false;

            if (frames.size() >= MAX_NUM_STACK_FRAMES)
                break;
        }

        SymCleanup(process);
    }

    void fail(Span<char> buff, const char* msg)
    {
        int curr = stbsp_snprintf(buff.data(), (int)buff.size(), "%s\n\n", msg);
        size_t bytesLeft = buff.size() - curr;
        if (bytesLeft <= 0)
            return;

        MemoryArena ma(1024 * 8);
        ArenaAllocator aa(ma);

        SmallVector<StackFrame, ArenaAllocator> stack(aa);
        stack_trace(stack);

        curr += stbsp_snprintf(buff.data() + curr, (int)bytesLeft, "Callstack: \n");
        bytesLeft = BUFF_SIZE - curr;
        if (bytesLeft <= 0)
            return;

        for (auto i = 0; i < std::min(stack.size(), MAX_NUM_STACK_FRAMES); i++)
        {
            if (stack[i].name.starts_with("dbg::fail"))
                continue;

            else if (stack[i].name.starts_with("ZetaRay::Util::ReportError"))
                continue;

            curr += stbsp_snprintf(buff.data() + curr, (int)bytesLeft, " - 0x%x: %s(%d) in %s\n",
                stack[i].address, stack[i].name.c_str(), stack[i].line, stack[i].module.c_str());

            bytesLeft = BUFF_SIZE - curr;

            if (bytesLeft <= 0)
                return;
        }
    }
}
#endif // _DEBUG


void ZetaRay::Util::ReportError(const char* title, const char* msg) noexcept
{
#ifdef _DEBUG
    char buff[BUFF_SIZE];

    dbg::fail(buff, msg);
    MessageBoxA(nullptr, buff, title, MB_ICONERROR | MB_OK);
#else
    MessageBoxA(nullptr, msg, title, MB_ICONERROR | MB_OK);
#endif // _DEBUG
}

void ZetaRay::Util::ReportErrorWin32(const char* file, int line, const char* call) noexcept
{
    char msg[256];
    stbsp_snprintf(msg, 256, "%s: %d\nPredicate: %s\nError code: %d", file, line, call, GetLastError());

#ifdef _DEBUG
    char buff[BUFF_SIZE];
    dbg::fail(buff, msg);
    MessageBoxA(nullptr, buff, "Win32 call failed", MB_ICONERROR | MB_OK);
#else
    MessageBoxA(nullptr, msg, "Win32 call failed", MB_ICONERROR | MB_OK);
#endif // _DEBUG
}

void ZetaRay::Util::DebugBreak() noexcept
{
	__debugbreak();
}

void ZetaRay::Util::Exit() noexcept
{
    exit(EXIT_FAILURE);
}
