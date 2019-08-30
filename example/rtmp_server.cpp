#ifndef _CRT_SECURE_NO_WARNINGS
#define _CRT_SECURE_NO_WARNINGS
#endif

#include "xop/RtmpServer.h"
#include "xop/RtmpPublisher.h"
#include "xop/H264Parser.h"
#include "net/EventLoop.h"

#define TEST_RTMP_PUSHER 1
#define PUSH_URL    "rtmp://127.0.0.1:1935/live/01"
#define PUSH_FILE   "./test.h264"

int test_rtmp_publisher(xop::EventLoop *eventLoop);

int main(int argc, char **argv)
{
    xop::EventLoop eventLoop;  

	/* rtmp server example */
    xop::RtmpServer server(&eventLoop, "0.0.0.0", 1935);    
	server.setChunkSize(60000);
	server.setGopCache(); /* enable gop cache */

#if TEST_RTMP_PUSHER
	std::thread t([&eventLoop] () {
		test_rtmp_publisher(&eventLoop);
	});
	t.detach();
#endif 

    eventLoop.loop(); 
    
    return 0;
}


class H264File
{
public:
	H264File(int bufSize = 5000000);
	~H264File();

	bool open(const char *path);
	void close();

	bool isOpened() const
	{
		return (m_file != NULL);
	}

	int readFrame(char *inBuf, int inBufSize, bool *bEndOfFrame);

private:
	FILE *m_file = NULL;
	char *m_buf = NULL;
	int m_bufSize = 0;
	int m_bytesUsed = 0;
	int m_count = 0;
};

H264File::H264File(int bufSize)
	: m_bufSize(bufSize)
{
	m_buf = new char[m_bufSize];
}

H264File::~H264File()
{
	delete m_buf;
}

bool H264File::open(const char *path)
{
	m_file = fopen(path, "rb");
	if (m_file == NULL)
	{
		return false;
	}

	return true;
}

void H264File::close()
{
	if (m_file)
	{
		fclose(m_file);
		m_file = NULL;
		m_count = 0;
		m_bytesUsed = 0;
	}
}

int H264File::readFrame(char *inBuf, int inBufSize, bool *bEndOfFrame)
{
	if (m_file == NULL)
	{
		return -1;
	}

	int bytesRead = (int)fread(m_buf, 1, m_bufSize, m_file);
	if (bytesRead == 0)
	{
		fseek(m_file, 0, SEEK_SET);
		m_count = 0;
		m_bytesUsed = 0;
		bytesRead = (int)fread(m_buf, 1, m_bufSize, m_file);
		if (bytesRead == 0)
		{
			this->close();
			return -1;
		}
	}

	bool bFindStart = false, bFindEnd = false;

	int i = 0, startCode = 3;
	*bEndOfFrame = false;
	for (i = 0; i < bytesRead - 5; i++)
	{
		if (m_buf[i] == 0 && m_buf[i + 1] == 0 && m_buf[i + 2] == 1)
		{
			startCode = 3;
		}
		else if (m_buf[i] == 0 && m_buf[i + 1] == 0 && m_buf[i + 2] == 0 && m_buf[i + 3] == 1)
		{
			startCode = 4;
		}
		else
		{
			continue;
		}

		if (((m_buf[i + startCode] & 0x1F) == 0x5 || (m_buf[i + startCode] & 0x1F) == 0x1) &&
			((m_buf[i + startCode + 1] & 0x80) == 0x80))
		{
			bFindStart = true;
			i += 4;
			break;
		}
	}

	for (; i < bytesRead - 5; i++)
	{
		if (m_buf[i] == 0 && m_buf[i + 1] == 0 && m_buf[i + 2] == 1)
		{
			startCode = 3;
		}
		else if (m_buf[i] == 0 && m_buf[i + 1] == 0 && m_buf[i + 2] == 0 && m_buf[i + 3] == 1)
		{
			startCode = 4;
		}
		else
		{
			continue;
		}

		if (((m_buf[i + startCode] & 0x1F) == 0x7) || ((m_buf[i + startCode] & 0x1F) == 0x8)
			|| ((m_buf[i + startCode] & 0x1F) == 0x6) || (((m_buf[i + startCode] & 0x1F) == 0x5
				|| (m_buf[i + startCode] & 0x1F) == 0x1) && ((m_buf[i + startCode + 1] & 0x80) == 0x80)))
		{
			bFindEnd = true;
			break;
		}
	}

	bool flag = false;
	if (bFindStart && !bFindEnd && m_count > 0)
	{
		flag = bFindEnd = true;
		i = bytesRead;
		*bEndOfFrame = true;
	}

	if (!bFindStart || !bFindEnd)
	{
		this->close();
		return -1;
	}

	int size = (i <= inBufSize ? i : inBufSize);
	memcpy(inBuf, m_buf, size);

	if (!flag)
	{
		m_count += 1;
		m_bytesUsed += i;
	}
	else
	{
		m_count = 0;
		m_bytesUsed = 0;
	}

	fseek(m_file, m_bytesUsed, SEEK_SET);
	return size;
}

int test_rtmp_publisher(xop::EventLoop *eventLoop)
{
	H264File h264File;
	if (!h264File.open(PUSH_FILE))
	{
		printf("Open %s failed.\n", PUSH_FILE);
		return -1;
	}

	/* push stream to local rtmp server */
	xop::MediaInfo mediaInfo;
	xop::RtmpPublisher publisher(eventLoop);
	publisher.setChunkSize(60000);
	if (publisher.openUrl(PUSH_URL) < 0)
	{
		printf("Open url %s failed.\n", PUSH_URL);
		return -1;
	}

	int bufSize = 500000;
	bool bEndOfFrame = false;
	bool hasSpsPps = false;
	uint8_t *frameBuf = new uint8_t[bufSize];

	while (1)
	{
		int frameSize = h264File.readFrame((char*)frameBuf, bufSize, &bEndOfFrame);
		if (frameSize > 0)
		{
			if (!hasSpsPps)
			{
				if (frameBuf[3] == 0x67 || frameBuf[4] == 0x67)
				{
					xop::Nal sps = xop::H264Parser::findNal(frameBuf, frameSize);
					if (sps.first != nullptr && sps.second != nullptr && *sps.first == 0x67)
					{
						mediaInfo.spsSize = (uint32_t)(sps.second - sps.first + 1);
						mediaInfo.sps.reset(new uint8_t[mediaInfo.spsSize]);
						memcpy(mediaInfo.sps.get(), sps.first, mediaInfo.spsSize);

						xop::Nal pps = xop::H264Parser::findNal(sps.second, frameSize - (int)(sps.second - frameBuf));
						if (pps.first != nullptr && pps.second != nullptr && *pps.first == 0x68)
						{
							mediaInfo.ppsSize = (uint32_t)(pps.second - pps.first + 1);
							mediaInfo.pps.reset(new uint8_t[mediaInfo.ppsSize]);
							memcpy(mediaInfo.pps.get(), pps.first, mediaInfo.ppsSize);

							hasSpsPps = true;
							publisher.setMediaInfo(mediaInfo); /* set sps pps */
							printf("Start rtmp pusher, play url: %s\n\n", PUSH_URL);
						}
					}
				}
			}
			else
			{
				publisher.pushVideoFrame(frameBuf, frameSize); /* send h.264 frame */
			}
		}

		std::this_thread::sleep_for(std::chrono::milliseconds(40));
	}
	
	delete frameBuf;
	return 0;
}