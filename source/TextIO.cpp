﻿#include "stdafx.h"
#include "TextIO.h"
#include "script.h"
#include "script_object.h"
#include "script_func_impl.h"
EXTERN_SCRIPT;

UINT g_ACP = GetACP(); // Requires a reboot to change.
#define INVALID_CHAR UorA(0xFFFD, '?')

#ifndef UNICODE

CPINFO GetACPInfo()
{
	CPINFO info;
	GetCPInfo(CP_ACP, &info);
	return info;
}
CPINFO g_ACPInfo = GetACPInfo();

// Benchmarks faster than _ismbblead_l with ACP locale:
bool IsLeadByteACP(BYTE b)
{
	// Benchmarks slightly faster without this check, even when MaxCharSize == 1:
	//if (g_ACPInfo.MaxCharSize > 1)
	for (int i = 0; i < _countof(g_ACPInfo.LeadByte) && g_ACPInfo.LeadByte[i]; i += 2)
		if (b >= g_ACPInfo.LeadByte[i] && b <= g_ACPInfo.LeadByte[i+1])
			return true;
	return false;
}

#endif



//
// TextStream
//
bool TextStream::Open(LPCTSTR aFileSpec, DWORD aFlags, UINT aCodePage)
{
	mLength = 0; // Set the default value here so _Open() can change it.
	if (!_Open(aFileSpec, aFlags))
		return false;

	SetCodePage(aCodePage);
	mFlags = aFlags;
	mLastWriteChar = 0;

	int mode = aFlags & ACCESS_MODE_MASK;
	if (mode == USEHANDLE)
		return true;
	if (mode != TextStream::WRITE) {
		// Detect UTF-8 and UTF-16LE BOMs
		if (mLength < 3)
			Read(TEXT_IO_BLOCK); // TEXT_IO_BLOCK vs 3 for consistency and average-case performance.
		mPos = mBuffer;
		if (mLength >= 2) {
			if (mBuffer[0] == 0xFF && mBuffer[1] == 0xFE) {
				mPosW += 1;
				SetCodePage(CP_UTF16);
			}
			else if (mBuffer[0] == 0xEF && mBuffer[1] == 0xBB) {
				if (mLength >= 3 && mBuffer[2] == 0xBF) {
					mPosA += 3;
					SetCodePage(CP_UTF8);
				}
			}
		}
	}
	if (mode == TextStream::WRITE || (mode == TextStream::APPEND || mode == TextStream::UPDATE) && _Length() == 0) {
		if (aFlags & BOM_UTF8)
			_Write("\xEF\xBB\xBF", 3);
		else if (aFlags & BOM_UTF16)
			_Write("\xFF\xFE", 2);
	}
	else if (mode == TextStream::APPEND)
	{
		mPos = NULL; // Without this, RollbackFilePointer() gets called later on and
		mLength = 0; // if the file had no UTF-8 BOM we end up in the wrong position.
		_Seek(0, SEEK_END);
	}

	return true;
}



DWORD TextStream::Read(LPTSTR aBuf, DWORD aBufLen, BOOL aReadLine)
{
	if (!PrepareToRead())
		return 0;

	DWORD target_used = 0;
	LPBYTE src, src_end;
	TCHAR dst[UorA(2,4)];
	int src_size; // Size of source character, in bytes.
	int dst_size; // Number of code units in destination character.

	UINT codepage = mCodePage; // For performance.
	int chr_size = (codepage == CP_UTF16) ? sizeof(WCHAR) : sizeof(CHAR);

	// This is set each iteration based on how many bytes we *need* to have in the buffer.
	// Avoid setting it higher than necessary since that can cause undesired effects with
	// non-file handles -  such as a console waiting for a second line of input when the
	// first line is waiting in our buffer.
	int next_size = chr_size;

	while (target_used < aBufLen)
	{
		// Ensure the buffer contains at least one CHAR/WCHAR, or all bytes of the next
		// character as determined by a previous iteration of the loop.  Note that Read()
		// only occurs if the buffer contains less than next_size bytes, and that this
		// check does not occur frequently due to buffering and the inner loop below.
		if (!ReadAtLeast(next_size) && !mLength)
			break;

		// Reset to default (see comments above).
		next_size = chr_size;

		// The following macro is used when there is insufficient data in the buffer,
		// to determine if more data can possibly be read in.  Using mLastRead should be
		// faster than AtEOF(), and more reliable with console/pipe handles.
		#define LAST_READ_HIT_EOF (mLastRead == 0)
		
		// Because we copy mPos into a temporary variable here and update mPos at the end of
		// each outer loop iteration, it is very important that ReadAtLeast() not be called
		// after this point.
		src = mPos;
		src_end = mBuffer + mLength; // Maint: mLength is in bytes.
		
		// Ensure there are an even number of bytes in the buffer if we are reading UTF-16.
		// This can happen (for instance) when dealing with binary files which also contain
		// UTF-16 strings, or if a UTF-16 file is missing its last byte.
		if (codepage == CP_UTF16 && ((src_end - src) & 1))
		{
			// Try to defer processing of the odd byte until the next byte is read.
			--src_end;
			// If it's the only byte remaining, the safest thing to do is probably to drop it
			// from the stream and output an invalid char so that the error can be detected:
			if (src_end == src)
			{
				mPos = NULL;
				mLength = 0;
				aBuf[target_used++] = INVALID_CHAR;
				break;
			}
		}

		for ( ; src < src_end && target_used < aBufLen; src += src_size)
		{
			if (codepage == CP_UTF16)
			{
				src_size = sizeof(WCHAR); // Set default (currently never overridden).
				LPWSTR cp = (LPWSTR)src;
				// There doesn't seem to be much need to give surrogate pairs special handling,
				// so the following is disabled for now.  Some "brute force" tests on Windows 7
				// showed that none of the ANSI code pages are capable of representing any of
				// the supplementary characters.  Even if we pass the full pair in a single call,
				// the result is the same as with two separate calls: "??".
				/*if (*cp >= 0xD800 && *cp <= 0xDBFF) // High surrogate.
				{
					if (src + 3 >= src_end && !LAST_READ_HIT_EOF)
					{
						// There should be a low surrogate following this, but since there's
						// not enough data in the buffer we need to postpone processing it.
						break;
 					}
					// Rather than discarding unpaired high/low surrogate code units, let them
					// through as though this is UCS-2, not UTF-16. The following check is not
					// necessary since low surrogates can't be misinterpreted as \r or \n:
					//if (cp[1] >= 0xDC00 && cp[1] <= 0xDFFF)
				}*/
#ifdef UNICODE
				*dst = *cp;
				dst_size = 1;
#else
				dst_size = WideCharToMultiByte(CP_ACP, 0, cp, 1, dst, _countof(dst), NULL, NULL);
#endif
			}
			else
			{
				src_size = 1; // Set default.
				if (*src < 0x80)
				{
					// No codepage conversion needed for ASCII chars.
					*dst = *(LPSTR)src;
					dst_size = 1;
				}
				else
				{
					if (codepage == CP_UTF8)
					{
						if ((*src & 0xE0) == 0xC0)
							src_size = 2;
						else if ((*src & 0xF0) == 0xE0)
							src_size = 3;
						else if ((*src & 0xF8) == 0xF0)
							src_size = 4;
						else { // Invalid in current UTF-8 standard.
							aBuf[target_used++] = INVALID_CHAR;
							continue;
						}
					}
					else if (IsLeadByte(*src))
						src_size = 2;
					// Otherwise, leave it at the default set above: 1.
					
					// Ensure that the expected number of bytes are available:
					if (src + src_size > src_end)
					{
						if (LAST_READ_HIT_EOF)
						{
							mLength = 0; // Discard all remaining data, since it appears to be invalid.
							src = NULL;  // mPos is set to this outside the inner loop.
							aBuf[target_used++] = INVALID_CHAR;
						}
						else
						{
							next_size = src_size;
							// Let the next iteration handle this char after reading more data.
							// If no more data is read, LAST_READ_HIT_EOF will be true and the
							// next iteration will produce INVALID_CHAR.
						}
						break;
					}
#ifdef UNICODE
					dst_size = MultiByteToWideChar(codepage, MB_ERR_INVALID_CHARS, (LPSTR)src, src_size, dst, _countof(dst));
#else
					if (codepage == g_ACP)
					{
						// This char doesn't require any conversion.
						*dst = *(LPSTR)src;
						if (src_size > 1) // Can only be 1 or 2 in this case.
							dst[1] = src[1];
						dst_size = src_size;
					}
					else
					{
						// Convert this single- or multi-byte char to Unicode.
						int wide_size;
						WCHAR wide_char[2];
						wide_size = MultiByteToWideChar(codepage, MB_ERR_INVALID_CHARS, (LPSTR)src, src_size, wide_char, _countof(wide_char));
						if (wide_size)
						{
							// Convert from Unicode to the system ANSI code page.
							dst_size = WideCharToMultiByte(CP_ACP, 0, wide_char, wide_size, dst, _countof(dst), NULL, NULL);
						}
						else
						{
							src_size = 1; // Seems best to drop only this byte, even if it appeared to be a lead byte.
							dst_size = 0; // Allow the check below to handle it.
						}
					}
#endif
				} // end (*src >= 0x80)
			}

			if (dst_size == 1)
			{
				if (*dst == '\n' || *dst == '\r')
				{
					if (*dst == '\r')
					{
						if (src + 2 * chr_size <= src_end)
						{
							int nextch = (codepage == CP_UTF16) ? LPWSTR(src)[1] : LPSTR(src)[1];
							if (nextch == '\n') // \r\n
							{
								src_size *= 2; // Increase it to two characters (\r and \n).
								if (!aReadLine)
								{
									if (  !(mFlags & EOL_CRLF)  )
									{
										// \r\n translation is disabled, so this \r\n should be written out as is.
										if (target_used + 2 > aBufLen)
										{
											// This \r\n wouldn't fit.  Seems best to leave the \r in the buffer for next
											// time rather than returning just the \r (even though \r\n translation isn't
											// enabled, it seems best to treat this \r\n as a single unit).
											mPos = src;
											aBuf[target_used] = '\0';
											return target_used;
										}
										aBuf[target_used++] = '\r';
									}
									aBuf[target_used++] = '\n';
									continue;
								}
							}
						}
						else if (!LAST_READ_HIT_EOF)
						{
							// There's not enough data in the buffer to determine if this is \r\n.
							// Let the next iteration handle this char after reading more data.
							next_size = 2 * chr_size;
							break;
						}
						// Since above didn't break or continue, this is an orphan \r.
						if (mFlags & EOL_ORPHAN_CR)
							*dst = '\n';
					}
					if (aReadLine)
					{
						// Our caller asked for one line, which we now have.
						mPos = src + src_size;
						aBuf[target_used++] = '\n'; // Caller expects a newline-terminated line, to distinguish empty lines from end of file.
						if (target_used < aBufLen)
							aBuf[target_used] = '\0';
						return target_used;
					}
				}

				// If we got to this point, dst contains a single TCHAR:
				aBuf[target_used++] = *dst;
			}
			else if (dst_size) // Multi-byte/surrogate pair.
			{
				if (target_used + dst_size > aBufLen)
				{
					// This multi-byte char/surrogate pair won't fit, so leave it in the file buffer.
					mPos = src;
					aBuf[target_used] = '\0';
					return target_used;
				}
				tmemcpy(aBuf + target_used, dst, dst_size);
				target_used += dst_size;
			}
			else
			{
				aBuf[target_used++] = INVALID_CHAR;
			}
		} // end for-loop which processes buffered data.
		if (src == src_end)
		{
			// Reset the buffer so that Read() can read a full block.
			mLength = 0;
			mPos = NULL;
		}
		else
			mPos = src;
	} // end for-loop which repopulates the buffer.
	if (target_used < aBufLen)
		aBuf[target_used] = '\0';
	// Otherwise, caller is responsible for reserving one char and null-terminating if needed.
	return target_used;
}



DWORD TextStream::Read(LPVOID aBuf, DWORD aBufLen)
{
	if (!PrepareToRead() || !aBufLen)
		return 0;

	DWORD target_used = 0;
	
	if (mPos)
	{
		DWORD data_in_buffer = (DWORD)(mBuffer + mLength - mPos);
		if (data_in_buffer >= aBufLen)
		{
			// The requested amount of data already exists in our buffer, so copy it over.
			memcpy(aBuf, mPos, aBufLen);
			if (data_in_buffer == aBufLen)
			{
				mPos = NULL; // No more data in buffer.
				mLength = 0; //
			}
			else
				mPos += aBufLen;
			return aBufLen;
		}
		
		// Consume all buffered data.  If there is none (i.e. mPos was somehow pointing at the
		// end of the buffer), it is crucial that we clear the buffer for the next section.
		memcpy(aBuf, mPos, data_in_buffer);
		target_used = data_in_buffer;
		mLength = 0;
		mPos = NULL;
	}

	LPBYTE target = (LPBYTE)aBuf + target_used;
	DWORD target_remaining = aBufLen - target_used;

	if (target_remaining < TEXT_IO_BLOCK)
	{
		Read(TEXT_IO_BLOCK);

		if (mLength <= target_remaining)
		{
			// All of the data read above will fit in the caller's buffer.
			memcpy(target, mBuffer, mLength);
			target_used += mLength;
			mLength = 0;
			// Since no data remains in the buffer, mPos can remain set to NULL.
			// UPDATE: If (mPos == mBuffer + mLength), it was not set to NULL above.
			mPos = NULL;
		}
		else
		{
			// Surplus data was read 
			memcpy(target, mBuffer, target_remaining);
			target_used += target_remaining;
			mPos = mBuffer + target_remaining;
		}
	}
	else
	{
		// The remaining data to be read exceeds the capacity of our buffer, so bypass it.
		target_used += _Read(target, target_remaining);
	}

	return target_used;
}



DWORD TextStream::Write(LPCTSTR aBuf, DWORD aBufLen)
// Returns the number of bytes aBuf took after performing applicable EOL and
// code page translations.  Since data is buffered, this is generally not the
// amount actually written to file.  Returns 0 on apparent critical failure,
// even if *some* data was written into the buffer and/or to file.
{
	if (!PrepareToWrite())
		return 0;

	if (aBufLen == 0)
	{
		aBufLen = (DWORD)_tcslen(aBuf);
		if (aBufLen == 0) // Below may rely on this having been checked.
			return 0;
	}
	
	DWORD bytes_flushed = 0; // Number of buffered bytes flushed to file; used to calculate our return value.

	LPCTSTR src;
	LPCTSTR src_end;
	int src_size;
	
	union {
		LPBYTE	dst;
		LPSTR	dstA;
		LPWSTR	dstW;
	};
	dst = mBuffer + mLength;
	
	// Allow enough space in the buffer for any one of the following:
	//	a 4-byte UTF-8 sequence
	//	a UTF-16 surrogate pair
	//	a carriage-return/newline pair
	LPBYTE dst_end = mBuffer + TEXT_IO_BLOCK - 4;

	for (src = aBuf, src_end = aBuf + aBufLen; ; )
	{
		// The following section speeds up writing of ASCII characters by copying as many as
		// possible in each iteration of the outer loop, avoiding certain checks that would
		// otherwise be made once for each char.  This relies on the fact that ASCII chars
		// have the same binary value (but not necessarily width) in every supported encoding.
		// EOL logic is also handled here, for performance.  An alternative approach which is
		// tidier and performs almost as well is to add (*src != '\n') to each loop's condition
		// and handle it after the loop terminates.
		if (mCodePage != CP_UTF16)
		{
			for ( ; src < src_end && !(*src & ~0x7F) && dst < dst_end; ++src)
			{
				if (*src == '\n' && (mFlags & EOL_CRLF) && ((src == aBuf) ? mLastWriteChar : src[-1]) != '\r')
					*dstA++ = '\r';
				*dstA++ = (CHAR)*src;
			}
		}
		else
		{
#ifdef UNICODE
			for ( ; src < src_end && dst < dst_end; ++src)
#else
			for ( ; src < src_end && !(*src & ~0x7F) && dst < dst_end; ++src) // No conversion needed for ASCII chars.
#endif
			{
				if (*src == '\n' && (mFlags & EOL_CRLF) && ((src == aBuf) ? mLastWriteChar : src[-1]) != '\r')
					*dstW++ = '\r';
				*dstW++ = (WCHAR)*src;
			}
		}

		if (dst >= dst_end)
		{
			DWORD len = (DWORD)(dst - mBuffer);
			if (_Write(mBuffer, len) < len)
			{
				// The following isn't done since there's no way for the caller to know
				// how much of aBuf was successfully translated or written to file, or
				// even how many bytes to expect due to EOL and code page translations:
				//if (written)
				//{
				//	// Since a later call might succeed, remove this data from the buffer
				//	// to prevent it from being written twice.  Note that some or all of
				//	// this data might've been buffered by a previous call.
				//	memmove(mBuffer, mBuffer + written, mLength - written);
				//	mLength -= written;
				//}
				// Instead, dump the contents of the buffer along with the remainder of aBuf,
				// then return 0 to indicate a critical failure.
				mLength = 0;
				return 0;
			}
			bytes_flushed += len;
			dst = mBuffer;
			continue; // If *src is ASCII, we want to use the high-performance mode (above).
		}

		if (src == src_end)
			break;

#ifdef UNICODE
		if (*src >= 0xD800 && *src <= 0xDBFF // i.e. this is a UTF-16 high surrogate.
			&& src + 1 < src_end // If this is at the end of the string, there is no low surrogate.
			&& src[1] >= 0xDC00 && src[1] <= 0xDFFF) // This is a complete surrogate pair.
#else
		if (IsLeadByteACP((BYTE)*src) && src + 1 < src_end) // src[1] is the second byte of this char.
#endif
			src_size = 2;
		else
			src_size = 1;

#ifdef UNICODE
		ASSERT(mCodePage != CP_UTF16); // An optimization above already handled UTF-16.
		dstA += WideCharToMultiByte(mCodePage, 0, src, src_size, dstA, 4, NULL, NULL);
		src += src_size;
#else
		if (mCodePage == g_ACP)
		{
			*dst++ = (BYTE)*src++;
			if (src_size == 2)
				*dst++ = (BYTE)*src++;
		}
		else
		{
			WCHAR wc;
			if (MultiByteToWideChar(CP_ACP, MB_ERR_INVALID_CHARS, src, src_size, &wc, 1))
			{
				if (mCodePage == CP_UTF16)
					*dstW++ = wc;
				else
					dstA += WideCharToMultiByte(mCodePage, 0, &wc, 1, (LPSTR)dst, 4, NULL, NULL);
			}
			src += src_size;
		}
#endif
	}

	mLastWriteChar = src_end[-1]; // So if this is \r and the next char is \n, don't make it \r\r\n.

	DWORD initial_length = mLength;
	mLength = (DWORD)(dst - mBuffer);
	return bytes_flushed + mLength - initial_length; // Doing it this way should perform better and result in smaller code than counting each byte put into the buffer.
}



DWORD TextStream::Write(LPCVOID aBuf, DWORD aBufLen)
{
	if (!PrepareToWrite())
		return 0;

	if (aBufLen < TEXT_IO_BLOCK - mLength) // There would be room for at least 1 byte after appending data.
	{
		// Buffer the data.
		memcpy(mBuffer + mLength, aBuf, aBufLen);
		mLength += aBufLen;
		return aBufLen;
	}
	else
	{
		// data is bigger than the remaining space in the buffer.  If (len < TEXT_IO_BLOCK*2 - mLength), we
		// could copy the first part of data into the buffer, flush it, then write the remainder into the
		// buffer to await more text to be buffered.  However, the need for a memcpy combined with the added
		// code size and complexity mean it probably isn't worth doing.
		if (mLength)
		{
			_Write(mBuffer, mLength);
			mLength = 0;
		}
		return _Write(aBuf, aBufLen);
	}
}



//
// TextFile
//
bool TextFile::_Open(LPCTSTR aFileSpec, DWORD &aFlags)
{
	_Close();
	DWORD dwDesiredAccess, dwShareMode, dwCreationDisposition;
	switch (aFlags & ACCESS_MODE_MASK) {
		case READ:
			dwDesiredAccess = GENERIC_READ;
			dwCreationDisposition = OPEN_EXISTING;
			break;
		case WRITE:
			dwDesiredAccess = GENERIC_WRITE;
			dwCreationDisposition = CREATE_ALWAYS;
			break;
		case APPEND:
		case UPDATE:
			dwDesiredAccess = GENERIC_WRITE | GENERIC_READ;
			dwCreationDisposition = OPEN_ALWAYS;
			break;
		case USEHANDLE:
			if (!GetFileType((HANDLE)aFileSpec))
				return false;
			mFile = (HANDLE)aFileSpec;
			return true;
	}
	dwShareMode = ((aFlags >> 8) & (FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE));

	if (*aFileSpec == '*')
	{
		// v1.1.17: Allow FileOpen("*", "r|w") to open stdin/stdout/stderr ("**" for stderr).
		// Can also be used to read script text from stdin, by passing "*" as the filename.
		DWORD nStdHandle = 0;
		switch (aFlags & ACCESS_MODE_MASK)
		{
		case APPEND:
			// Allow FileAppend to write to stdout/stderr via TextStream.
			aFlags = (aFlags & ~ACCESS_MODE_MASK) | READ;
		case WRITE:
			if (!aFileSpec[1])
				nStdHandle = STD_OUTPUT_HANDLE;
			else if (aFileSpec[1] == '*' && !aFileSpec[2])
				nStdHandle = STD_ERROR_HANDLE;
			break;
		case READ:
			if (!aFileSpec[1])
				nStdHandle = STD_INPUT_HANDLE;
			break;
		}
		if (nStdHandle) // It was * or ** and not something invalid like *Somefile.
		{
			HANDLE hstd = GetStdHandle(nStdHandle);
			if (hstd == NULL)// || !DuplicateHandle(GetCurrentProcess(), hstd, GetCurrentProcess(), &hstd, 0, FALSE, DUPLICATE_SAME_ACCESS))
			{
				SetLastError(ERROR_INVALID_HANDLE); // Avoid "The operation completed successfully".
				return false;
			}
			aFlags = (aFlags & ~ACCESS_MODE_MASK) | USEHANDLE; // Avoid calling CloseHandle(), since we don't own it.
			mFile = hstd; // Only now that we know it's not NULL.
			return true;
		}
		// For any case not handled above, such as WRITE|READ combined or *** or *Somefile,
		// it should be detected as an error below by CreateFile() failing (or if not, it's
		// somehow valid and should not be treated as an error).
	}
	
	// FILE_FLAG_SEQUENTIAL_SCAN is set, as sequential accesses are quite common for text files handling.
	mFile = CreateFile(aFileSpec, dwDesiredAccess, dwShareMode, NULL, dwCreationDisposition,
		(aFlags & (EOL_CRLF | EOL_ORPHAN_CR)) ? FILE_FLAG_SEQUENTIAL_SCAN : 0, NULL);

	return mFile != INVALID_HANDLE_VALUE;
}

void TextFile::_Close()
{
	if (mFile != INVALID_HANDLE_VALUE) {
		if ((mFlags & ACCESS_MODE_MASK) != USEHANDLE)
			CloseHandle(mFile);
		mFile = INVALID_HANDLE_VALUE;
	}
}

DWORD TextFile::_Read(LPVOID aBuffer, DWORD aBufSize)
{
	DWORD dwRead = 0;
	ReadFile(mFile, aBuffer, aBufSize, &dwRead, NULL);
	return dwRead;
}

DWORD TextFile::_Write(LPCVOID aBuffer, DWORD aBufSize)
{
	DWORD dwWritten = 0;
	WriteFile(mFile, aBuffer, aBufSize, &dwWritten, NULL);
	return dwWritten;
}

bool TextFile::_Seek(__int64 aDistance, int aOrigin)
{
	return !!SetFilePointerEx(mFile, *((PLARGE_INTEGER) &aDistance), NULL, aOrigin);
}

__int64 TextFile::_Tell() const
{
	LARGE_INTEGER in = {0}, out;
	return SetFilePointerEx(mFile, in, &out, FILE_CURRENT) ? out.QuadPart : -1;
}

__int64 TextFile::_Length() const
{
	LARGE_INTEGER size;
	GetFileSizeEx(mFile, &size);
	return size.QuadPart;
}


// FileObject: exports TextFile interfaces to the scripts.
class FileObject : public Object
{
	FileObject() {}
	~FileObject() {}

	enum MemberID {
		// methods
		M_Read,
		M_Write,
		M_ReadLine,
		M_WriteLine,
		M_RawRead,
		M_RawWrite,
		M_Close,
		M_Seek,
		// properties
		P_Pos,
		P_Length,
		P_AtEOF,
		P_Handle,
		P_Encoding
	};

	static ObjectMember sMembers[];
	static Object *sPrototype;

	enum NumReadWriteFlags
	{
		F_SIZE_MASK = 0xF,
		F_READ = 0x10,
		F_WRITE = 0,
		F_SIGNED = 0x20,
		F_UNSIGNED = 0,
		F_FLOAT = 0x40
	};

	ResultType NumReadWrite(ResultToken &aResultToken, int aID, int aFlags, ExprTokenType *aParam[], int aParamCount)
	{
		const bool reading = aID & F_READ;
		const BOOL is_signed = aID & F_SIGNED, is_float = aID & F_FLOAT;
		const DWORD size = aID & F_SIZE_MASK;

		union {
			__int64 i8;
			int i4;
			short i2;
			char i1;
			double d;
			float f;
		} buf;

		if (reading)
		{
			buf.i8 = 0;
			if ( !mFile.Read(&buf, size) )
				_o_return_empty; // Fail.

			if (is_float)
			{
				_o_return((size == 4) ? buf.f : buf.d);
			}
			else
			{
				if (is_signed)
				{
					// sign-extend to 64-bit
					switch (size)
					{
					case 4: buf.i8 = buf.i4; break;
					case 2: buf.i8 = buf.i2; break;
					case 1: buf.i8 = buf.i1; break;
					//case 8: not needed.
					}
				}
				//else it's unsigned. No need to zero-extend thanks to init done earlier.
				_o_return(buf.i8);
			}
		}
		else
		{
			ExprTokenType &token_to_write = *aParam[0];

			if (is_float)
			{
				buf.d = TokenToDouble(token_to_write);
				if (size == 4)
					buf.f = (float)buf.d;
			}
			else
			{
				if (size == 8 && !is_signed && !IS_NUMERIC(token_to_write.symbol))
					buf.i8 = (__int64)ATOU64(TokenToString(token_to_write)); // For comments, search for ATOU64 in BIF_DllCall().
				else
					buf.i8 = TokenToInt64(token_to_write);
			}

			DWORD bytes_written = mFile.Write(&buf, size);
			// Return bytes_written even if it is 0:
			_o_return(bytes_written);
		}
	}

	ResultType Invoke(ResultToken &aResultToken, int aID, int aFlags, ExprTokenType *aParam[], int aParamCount)
	{
		aResultToken.symbol = SYM_INTEGER; // Set default return type -- the most common cases return integer.

		auto member = MemberID(aID);
		switch (member)
		{
		case M_Read:
			{
				DWORD length;
				if (aParamCount)
					length = (DWORD)ParamIndexToInt64(0);
				else
					length = (DWORD)(mFile.Length() - mFile.Tell()); // We don't know the actual number of characters these bytes will translate to, but this should be sufficient.
				if (length == -1)
					break; // Fail.
				if (!TokenSetResult(aResultToken, NULL, length)) // Only after checking length above: TokenSetResult requires non-NULL aResult if aResultLength == -1.
					return FAIL;
				length = mFile.Read(aResultToken.marker, length);
				aResultToken.symbol = SYM_STRING;
				aResultToken.marker[length] = '\0';
				aResultToken.marker_length = length; // Update marker_length to the actual number of characters read.
				return OK;
			}
			break;
		
		case M_ReadLine:
			{	// See above for comments.
				if (!TokenSetResult(aResultToken, NULL, READ_FILE_LINE_SIZE))
					return FAIL;
				DWORD length = mFile.ReadLine(aResultToken.marker, READ_FILE_LINE_SIZE - 1);
				aResultToken.symbol = SYM_STRING;
				if (length && aResultToken.marker[length - 1] == '\n')
					--length;
				aResultToken.marker[length] = '\0';
				aResultToken.marker_length = length;
				return OK;
			}
			break;

		case M_Write:
		case M_WriteLine:
			{
				DWORD bytes_written = 0;
				size_t chars_to_write = 0;
				if (aParamCount)
				{
					LPTSTR param1 = ParamIndexToString(0, _f_number_buf, &chars_to_write);
					bytes_written = mFile.Write(param1, (DWORD)chars_to_write);
				}
				if (member == M_WriteLine && (bytes_written || !chars_to_write)) // i.e. don't attempt it if above failed.
				{
					bytes_written += mFile.Write(_T("\n"), 1);
				}
				aResultToken.value_int64 = bytes_written;
				return OK;
			}
			break;

		case M_RawRead:
		case M_RawWrite:
			{
				bool reading = member == M_RawRead;

				LPVOID target;
				DWORD max_size;
				ExprTokenType &target_token = *aParam[0];
				switch (target_token.symbol)
				{
				case SYM_STRING:
					if (reading)
						_o_throw(ERR_PARAM1_INVALID); // Can't read into a read-only string.
					target = target_token.marker;
					max_size = (DWORD)(target_token.marker_length + 1) * sizeof(TCHAR); // Allow +1 to write the null-terminator (but it won't be written by default if size is omitted).
					break;
				default:
					if (IObject *obj = TokenToObject(target_token))
					{
						size_t ptr, size;
						GetBufferObjectPtr(aResultToken, obj, ptr, size);
						if (aResultToken.Exited())
							return aResultToken.Result();
						target = (LPVOID)ptr;
						max_size = (DWORD)size;
					}
					else if (TokenIsPureNumeric(target_token) == PURE_INTEGER)
					{
						target = (LPVOID)TokenToInt64(target_token);
						max_size = ~0; // Unknown; perform no validation.
					}
					else
						target = 0;
					if ((size_t)target >= 65536) // Basic sanity check relying on the fact that Win32 platforms reserve the first 64KB of address space.
						break;
					// Otherwise, it's invalid:
					_o_throw(ERR_PARAM1_INVALID);
				}

				DWORD size;
				if (ParamIndexIsOmitted(1))
				{
					if (max_size == ~0) // Param #1 was an address.
						_o_throw(ERR_PARAM2_REQUIRED); // (in this case).
					size = max_size;
					if (!reading)
						// Default to the byte count of the binary string, excluding the null-terminator.
						size -= sizeof(TCHAR);
				}
				else
				{
					size = (DWORD)ParamIndexToInt64(1);
					if (size > max_size) // Implies max_size != ~0.
						_o_throw(ERR_PARAM2_INVALID); // Invalid size (param #2).
				}

				DWORD result;
				if (reading)
					result = mFile.Read(target, size);
				else
					result = mFile.Write(target, size);
				aResultToken.value_int64 = result;
				return OK;
			}
			break;

		case P_Pos:
			if (IS_INVOKE_GET)
			{
				aResultToken.value_int64 = mFile.Tell();
				return OK;
			}
			else
			{
		case M_Seek:
				__int64 distance = ParamIndexToInt64(0);
				int origin;
				if (aParamCount > 1)
					origin = ParamIndexToInt(1);
				else // Defaulting to SEEK_END when distance is negative seems more useful than allowing it to be interpreted as an unsigned value (> 9.e18 bytes).
					origin = (distance < 0) ? SEEK_END : SEEK_SET;

				aResultToken.value_int64 = mFile.Seek(distance, origin);
				return OK;
			}
			break;


		case P_Length:
			if (IS_INVOKE_GET) 
			{
				aResultToken.value_int64 = mFile.Length();
				return OK;
			}
			else
			{
				if (-1 != (aResultToken.value_int64 = mFile.Length(ParamIndexToInt64(0))))
					return OK;
				// Otherwise, fall through:
			}
			break;

		case P_AtEOF:
			aResultToken.value_int64 = mFile.AtEOF();
			return OK;
		
		case P_Handle:
			aResultToken.value_int64 = (UINT_PTR) mFile.Handle();
			return OK;

		case P_Encoding:
		{
			// Encoding: UTF-8, UTF-16 or CPnnn.  The -RAW suffix (CP_AHKNOBOM) is not supported; it is normally
			// stripped out when the file is opened, so passing it to SetCodePage() would break encoding/decoding
			// of non-ASCII characters (and did in v1.1.15.03 and earlier).  Although it could be detected/added
			// via TextStream::mFlags, this isn't done because:
			//  - It would only tell us whether the script passed "-RAW", not whether the file really has a BOM.
			//  - It's questionable which behaviour is more more useful, but excluding "-RAW" is definitely simpler.
			//  - Existing scripts may rely on File.Encoding not returning "-RAW".
			UINT codepage;
			if (IS_INVOKE_SET)
			{
				codepage = Line::ConvertFileEncoding(*aParam[0]);
				if (codepage == -1)
					_o_throw(ERR_INVALID_VALUE);
				mFile.SetCodePage(codepage & ~CP_AHKNOBOM); // Ignore "-RAW" by removing the CP_AHKNOBOM flag; see comments above.
				return OK;
			}
			LPTSTR name;
			codepage = mFile.GetCodePage();
			// There's no need to check for the CP_AHKNOBOM flag here because it's stripped out when the file is opened.
			switch (codepage)
			{
			// GetCodePage() returns the value of GetACP() in place of CP_ACP, so this case is not needed:
			//case CP_ACP:  name = _T("");  break;
			case CP_UTF8:	name = _T("UTF-8");  break;
			case CP_UTF16:	name = _T("UTF-16"); break;
			default:
				// Although we could check codepage == GetACP() and return blank in that case, there's no way
				// to know whether something like "CP0" or the actual codepage was passed to FileOpen, so just
				// return "CPn" when none of the cases above apply:
				name = aResultToken.buf;
				name[0] = _T('C');
				name[1] = _T('P');
				_itot(codepage, name + 2, 10);
			}
			aResultToken.symbol = SYM_STRING;
			aResultToken.marker = name;
			return OK;
		}

		case M_Close:
			mFile.Close();
			return OK;
		}
		
		// Since above didn't return, an error must've occurred.
		aResultToken.symbol = SYM_STRING;
		aResultToken.marker = _T(""); // Already set in most cases, but done here for maintainability.
		return OK;
	}

	TextFile mFile;
	
public:
	static FileObject *Open(LPCTSTR aFileSpec, DWORD aFlags, UINT aCodePage);
};


ObjectMember FileObject::sMembers[] =
{
	Object_Method (Close, 0, 0),
	Object_Method (RawRead, 1, 2),
	Object_Method (RawWrite, 1, 2),
	Object_Method (Read, 0, 1),
	Object_Method_(ReadChar, 0, 0, NumReadWrite, F_READ | F_SIGNED | 1),
	Object_Method_(ReadDouble, 0, 0, NumReadWrite, F_READ | F_FLOAT | 8),
	Object_Method_(ReadFloat, 0, 0, NumReadWrite, F_READ | F_FLOAT | 4),
	Object_Method_(ReadInt, 0, 0, NumReadWrite, F_READ | F_SIGNED | 4),
	Object_Method_(ReadInt64, 0, 0, NumReadWrite, F_READ | F_SIGNED | 8),
	Object_Method (ReadLine, 0, 0),
	Object_Method_(ReadShort, 0, 0, NumReadWrite, F_READ | F_SIGNED | 2),
	Object_Method_(ReadUChar, 0, 0, NumReadWrite, F_READ | F_UNSIGNED | 1),
	Object_Method_(ReadUInt, 0, 0, NumReadWrite, F_READ | F_UNSIGNED | 4),
	Object_Method_(ReadUShort, 0, 0, NumReadWrite, F_READ | F_UNSIGNED | 2),
	Object_Method (Seek, 1, 2),
	Object_Method (Write, 1, 1),
	Object_Method_(WriteChar, 1, 1, NumReadWrite, F_WRITE | F_SIGNED | 1),
	Object_Method_(WriteDouble, 1, 1, NumReadWrite, F_WRITE | F_FLOAT | 8),
	Object_Method_(WriteFloat, 1, 1, NumReadWrite, F_WRITE | F_FLOAT | 4),
	Object_Method_(WriteInt, 1, 1, NumReadWrite, F_WRITE | F_SIGNED | 4),
	Object_Method_(WriteInt64, 1, 1, NumReadWrite, F_WRITE | F_SIGNED | 8),
	Object_Method (WriteLine, 0, 1),
	Object_Method_(WriteShort, 1, 1, NumReadWrite, F_WRITE | F_SIGNED | 2),
	Object_Method_(WriteUChar, 1, 1, NumReadWrite, F_WRITE | F_UNSIGNED | 1),
	Object_Method_(WriteUInt, 1, 1, NumReadWrite, F_WRITE | F_UNSIGNED | 4),
	Object_Method_(WriteUShort, 1, 1, NumReadWrite, F_WRITE | F_UNSIGNED | 2),
	Object_Property_get    (AtEOF),
	Object_Property_get_set(Encoding),
	Object_Property_get    (Handle),
	Object_Property_get_set(Length),
	Object_Property_get_set(Pos)
};

Object *FileObject::sPrototype = Object::CreatePrototype(_T("File"), Object::sPrototype, sMembers, _countof(sMembers));

FileObject *FileObject::Open(LPCTSTR aFileSpec, DWORD aFlags, UINT aCodePage)
{
	FileObject *fileObj = new FileObject();
	fileObj->SetBase(sPrototype);
	if (fileObj && fileObj->mFile.Open(aFileSpec, aFlags, aCodePage))
		return fileObj;
	fileObj->Release();
	return NULL;
}


BIF_DECL(BIF_FileOpen)
{
	DWORD aFlags;
	UINT aEncoding;

	if (TokenIsNumeric(*aParam[1]))
	{
		aFlags = (DWORD) TokenToInt64(*aParam[1]);
	}
	else
	{
		LPCTSTR sflag = TokenToString(*aParam[1], aResultToken.buf);

		sflag = omit_leading_whitespace(sflag); // For consistency with the loop below.

		// Access mode must come first:
		switch (tolower(*sflag))
		{
		case 'r':
			if (tolower(sflag[1]) == 'w')
			{
				aFlags = TextStream::UPDATE;
				++sflag;
			}
			else
				aFlags = TextStream::READ;
			break;
		case 'w': aFlags = TextStream::WRITE; break;
		case 'a': aFlags = TextStream::APPEND; break;
		case 'h': aFlags = TextStream::USEHANDLE; break;
		default:
			// Invalid flag.
			goto invalid_param;
		}
		
		// Default to not locking file, for consistency with fopen/standard AutoHotkey and because it seems best for flexibility.
		aFlags |= TextStream::SHARE_ALL;

		for (++sflag; *sflag; ++sflag)
		{
			switch (ctolower(*sflag))
			{
			case '\n': aFlags |= TextStream::EOL_CRLF; break;
			case '\r': aFlags |= TextStream::EOL_ORPHAN_CR; break;
			case ' ':
			case '\t':
				// Allow spaces and tabs for readability.
				break;
			case '-':
				for (++sflag; ; ++sflag)
				{
					switch (ctolower(*sflag))
					{
					case 'r': aFlags &= ~TextStream::SHARE_READ; continue;
					case 'w': aFlags &= ~TextStream::SHARE_WRITE; continue;
					case 'd': aFlags &= ~TextStream::SHARE_DELETE; continue;
					// Whitespace not allowed here.  Outer loop allows "-r -w" but not "-r w".
					}
					if (sflag[-1] == '-')
						// Let "-" on its own be equivalent to "-rwd".
						aFlags &= ~TextStream::SHARE_ALL;
					break;
				}
				--sflag; // Point sflag at the last char of this option.  Outer loop will do ++sflag.
				break;
			default:
				// Invalid flag.
				goto invalid_param;
			}
		}
	}

	if (aParamCount > 2)
	{
		aEncoding = Line::ConvertFileEncoding(*aParam[2]);
		if (aEncoding == -1)
			goto invalid_param;
	}
	else aEncoding = g->Encoding;
	
	ASSERT( (~CP_AHKNOBOM) == CP_AHKCP );
	// aEncoding may include CP_AHKNOBOM, in which case below will not add BOM_UTFxx flag.
	if (aEncoding == CP_UTF8)
		aFlags |= TextStream::BOM_UTF8;
	else if (aEncoding == CP_UTF16)
		aFlags |= TextStream::BOM_UTF16;

	LPTSTR aFileName;
	if ((aFlags & TextStream::ACCESS_MODE_MASK) == TextStream::USEHANDLE)
		aFileName = (LPTSTR)(HANDLE)TokenToInt64(*aParam[0]);
	else
		aFileName = TokenToString(*aParam[0], aResultToken.buf);

	aResultToken.object = FileObject::Open(aFileName, aFlags, aEncoding & CP_AHKCP);
	if (aResultToken.object)
		aResultToken.symbol = SYM_OBJECT;
	else
		aResultToken.Win32Error();
	return;

invalid_param:
	_f_throw(ERR_PARAM_INVALID);
}


UINT Line::ConvertFileEncoding(ExprTokenType &aToken)
{
	if (TokenIsNumeric(aToken))
	{
		UINT cp = (UINT)TokenToInt64(aToken);
		return IsValidFileCodePage(cp) ? cp : -1;
	}
	return ConvertFileEncoding(TokenToString(aToken));
}


//
// TextMem
//
bool TextMem::_Open(LPCTSTR aFileSpec, DWORD &aFlags)
{
	ASSERT( (aFlags & ACCESS_MODE_MASK) == TextStream::READ ); // Only read mode is supported.

	if (mData.mOwned && mData.mBuffer)
		free(mData.mBuffer);
	mData = *(Buffer *)aFileSpec; // Struct copy.
	mDataPos = (LPBYTE)mData.mBuffer;
	mPos = NULL; // Discard temp buffer contents, if any.
	mLength = 0;
	return true;
}

void TextMem::_Close()
{
	if (mData.mBuffer) {
		if (mData.mOwned)
			free(mData.mBuffer);
		mData.mBuffer = NULL;
	}
}

DWORD TextMem::_Read(LPVOID aBuffer, DWORD aBufSize)
{
	DWORD remainder = (DWORD)((LPBYTE)mData.mBuffer + mData.mLength - mDataPos);
	if (aBufSize > remainder)
		aBufSize = remainder;
	memmove(aBuffer, mDataPos, aBufSize);
	mDataPos += aBufSize;
	return aBufSize;
}

DWORD TextMem::_Write(LPCVOID aBuffer, DWORD aBufSize)
{
	return 0;
}

bool TextMem::_Seek(__int64 aDistance, int aOrigin)
{
	return false;
}

__int64 TextMem::_Tell() const
{
	return (__int64) (mDataPos - (LPBYTE)mData.mBuffer);
}

__int64 TextMem::_Length() const
{
	return mData.mLength;
}
