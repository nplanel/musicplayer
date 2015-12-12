
#include "MP3Plugin.h"
#include "../../chipplayer.h"

#include <coreutils/utils.h>
#include <coreutils/file.h>

#include <mpg123.h>
//#include <curl/curl.h>

#include <coreutils/thread.h>

#include <set>
#include <unordered_map>

#ifdef EMSCRIPTEN
void srandom(unsigned int _seed)  { srand(_seed); }
long int random() { return rand(); }
#endif

using namespace std;
using namespace utils;

namespace chipmachine {

class MP3Player : public ChipPlayer {
public:

	MP3Player() {
		int err = mpg123_init();
		mp3 = mpg123_new(NULL, &err);
		bytesPut = 0;
		streamDone = false;
	}

	bool setParameter(const std::string  &param, int32_t v) override {
		if(!opened) {
			if(mpg123_open_feed(mp3) != MPG123_OK)
				throw player_exception("Could open MP3");
			opened = true;
		}
		if(param == "icy-interval") {
			LOGD("ICY INTERVAL %d", v);
			//mpg123_param(mp3, MPG123_ICY_INTERVAL, v, 0);
			metaInterval = v;
			return true;
		}
		if(param == "size") {
			mpg123_set_filesize(mp3, v);
			return true;
		}
		return false;
	}
	MP3Player(const std::string &fileName) {
		int err = mpg123_init();
		mp3 = mpg123_new(NULL, &err);

		if(mpg123_open(mp3, fileName.c_str()) != MPG123_OK)
			throw player_exception("Could open MP3");
		bytesPut = 1;
		int encoding = 0;
		if(mpg123_getformat(mp3, &rate, &channels, &encoding) != MPG123_OK)
			throw player_exception("Could not get format");
		LOGD("%d %d %d", rate, channels, encoding);
		mpg123_format_none(mp3);

		//mpg123_scan(mp3);
		checkMeta();

		mpg123_format(mp3, 44100, channels, encoding);
		//buf_size = 32768;
		//buffer = new unsigned char [buf_size];
	}

	~MP3Player() override {
		//delete [] buffer;
		if(mp3) {
			mpg123_close(mp3);
			mpg123_delete(mp3);
		}
		mpg123_exit();
	}

	void checkMeta() {

		if(!gotLength) {
			length = mpg123_length(mp3);
			if(length > 0) {
				LOGD("L %d T %f S %d", length, mpg123_tpf(mp3), mpg123_spf(mp3));
				length = length / mpg123_spf(mp3) * mpg123_tpf(mp3);
				gotLength = true;
				LOGD("MP3 LENGTH %ds", length);
				setMeta("length", length);
			}
		}

		int meta = mpg123_meta_check(mp3);
		mpg123_id3v1 *v1;
		mpg123_id3v2 *v2;
		if(meta & MPG123_ICY) {
			char *icydata;
			if(mpg123_icy(mp3, &icydata) == MPG123_OK) {
				LOGD("ICY:%s", icydata);
			}

		}
		if((meta & MPG123_NEW_ID3) && mpg123_id3(mp3, &v1, &v2) == MPG123_OK) {

			LOGV("New metadata");

			if(v2 && v2->title) {

				string msg;
				for(int i=0; i<v2->comments; i++) {
					if(msg.length())
						msg = msg + " ";
					msg = msg + v2->comment_list[i].text.p;
				}

				setMeta("title", htmldecode(v2->title->p),
					"composer", v2->artist ? htmldecode(v2->artist->p) : "",
					"message", msg,
					"format", "MP3",
					"length", length,
					"channels", channels);
			} else
			if(v1) {
				setMeta("title", htmldecode(v1->title),
					"composer", htmldecode(v1->artist),
					"message", v1->comment,
					"format", "MP3",
					"length", length,
					"channels", channels);
			} else {
				setMeta(
					"format", "MP3",
					"length", length,
					"channels", channels);
			}
		}
		if(meta)
			mpg123_meta_free(mp3);
	}

	virtual void putStream(const uint8_t *source, int size) override {
		lock_guard<mutex> {m};
		if(!opened) {
			if(mpg123_open_feed(mp3) != MPG123_OK)
				throw player_exception("Could open MP3");
			opened = true;
		}
		if(!source) {
			if(size <= 0)
				streamDone = true;
			//else
			//	mpg123_set_filesize(mp3, size);
			return;
		}
		
		do {

			if(metaInterval > 0 && metaCounter + size > metaInterval) {
				// This batch includes start of meta block
				int pos = metaInterval - metaCounter;
				metaSize = source[pos] * 16;

				LOGV("METASIZE %d at offset %d", metaSize, pos);

				if(pos > 0)
					mpg123_feed(mp3, source, pos);
				source += (pos+1);
				size -= (pos+1);
				bytesPut += (pos+1);
				metaCounter = 0;
				icyPtr = icyData;
			}
		
			if(metaSize > 0) {
				int metaBytes = size > metaSize ? metaSize : size;
				LOGD("Metabytes %d", metaBytes);

				memcpy(icyPtr, source, metaBytes);
				icyPtr += metaBytes;
				*icyPtr = 0;

				size -= metaBytes;
				source += metaBytes;
				metaSize -= metaBytes;

				if(metaSize <= 0) {
					LOGD("META: %s", icyData);
					icyPtr = icyData;

					auto parts = split(string(icyData), ";");
					for(const auto &p : parts) {
						auto data = split(p, "=", 2);
						if(data.size() == 2) {
							if(data[0] == "StreamTitle")
								setMeta("sub_title", data[1].substr(1, data[1].length()-2));
						}
					}
					
				}

			}
		} while(metaInterval > 0 && metaCounter + size > metaInterval);

		if(size > 0) {
			mpg123_feed(mp3, source, size);
			if(metaInterval > 0)
				metaCounter += size;
		}

		bytesPut += size;
		int bytesRead = mpg123_framepos(mp3);

		int inBuffer = bytesPut - bytesRead;

		checkMeta();
	}

	virtual int getSamples(int16_t *target, int noSamples) override {
		size_t done = 0;
		lock_guard<mutex> {m};
		if(bytesPut == 0)
			return 0;
		int err = mpg123_read(mp3, (unsigned char*)target, noSamples*2, &done);
		if(err == MPG123_NEW_FORMAT)
			return done/2;
		else
		if(err == MPG123_NEED_MORE) {
			if(streamDone)
				return -1;
		} else if(err < 0)
			return err;
		return done/2;
	}

	virtual bool seekTo(int song, int seconds) override {
		return false;
	}

private:
	mpg123_handle *mp3;
	//size_t buf_size;
	//unsigned char *buffer;
	long rate;
	int channels;
	//thread httpThread;
	mutex m;
	bool gotLength = false;
	bool gotMeta = false;
	int length;
	int bytesPut;
	bool streamDone;
	bool opened = false;
	int metaInterval = -1;
	int metaSize = 0;
	int metaCounter = 0;
	char icyData[16*256+1];
	char *icyPtr;
};

bool MP3Plugin::canHandle(const std::string &name) {
	auto ext = utils::path_extension(name);
	return ext == "mp3";
}

ChipPlayer *MP3Plugin::fromFile(const std::string &fileName) {
	return new MP3Player { fileName };
};

ChipPlayer *MP3Plugin::fromStream() {
	return new MP3Player();
}


}
