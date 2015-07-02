
#include "UADEPlugin.h"

#include "../../chipplayer.h"

#include <coreutils/utils.h>
#include <coreutils/file.h>
#include <coreutils/log.h>
#include <uade/uade.h>
#include <unistd.h>
#include <set>
#include <unordered_map>

/*
extern "C" {
	void init_uade();
	int get_samples(uint8_t *target, int bytes);
	int play_song(const char *name);
	void exit_song();
	void set_song(int song);
}*/

namespace chipmachine {

using namespace std;
using namespace utils;

class UADEPlayer : public ChipPlayer {
public:
	UADEPlayer(const std::string &dataDir) : dataDir(dataDir), valid(false), state(nullptr)  {
	}

	static struct uade_file *amigaloader(const char *name, const char *playerdir, void *context, struct uade_state *state) {
		LOGD("Trying to load '%s' from '%s'", name, playerdir);
		UADEPlayer *up = static_cast<UADEPlayer*>(context);
		string fileName = name;
		if(path_suffix(fileName) == "music") {
			fileName = path_directory(fileName) + "/" + up->baseName + "." + path_prefix(fileName);
			LOGD("Translated back to '%s'", fileName);
		} else if(up->currentFileName.find(fileName) == 0) {
			LOGD("Restoring filename %s back to '%s'", fileName, up->currentFileName);
			fileName = up->currentFileName;
		}

		struct uade_file *f = uade_load_amiga_file(fileName.c_str(), playerdir, state);
		return f;
	}

	bool load(string fileName) {

		char currdir[2048];
		if(!getcwd(currdir, sizeof(currdir)))
			return false;

		//auto path = current_exe_path() + ":" + File::getAppDir();
		//File ff = File::findFile(path, "data/uade/uaerc");
		//LOGD("Found %s", ff.getDirectory());
		int ok = chdir(dataDir.c_str());

		struct uade_config *config = uade_new_config();
		uade_config_set_option(config, UC_ONE_SUBSONG, NULL);
		uade_config_set_option(config, UC_IGNORE_PLAYER_CHECK, NULL);
		uade_config_set_option(config, UC_NO_EP_END, NULL);
		//uade_config_set_option(config, UC_VERBOSE, "true");
		state = uade_new_state(config);
	
		musicStopped = false;

		uade_set_amiga_loader(UADEPlayer::amigaloader, this, state);
		if(path_suffix(fileName) == "mdat") {
			baseName = path_basename(fileName);
			string uadeFileName = path_directory(fileName) + "/" + path_extension(fileName) + "." + "music";
			LOGD("Translated %s to %s", fileName, uadeFileName);
			File file { uadeFileName };
			File file2 { fileName };
			file.copyFrom(file2);
			file.close();
			fileName = uadeFileName;
		} 

		currentFileName = fileName;

		if(uade_play(fileName.c_str(), -1, state) == 1) {
			songInfo = uade_get_song_info(state);
			const char *modname = songInfo->modulename;
			if(strcmp(modname, "<no songtitle>") == 0)
				modname = "";
			setMeta(
				"songs", songInfo->subsongs.max - songInfo->subsongs.min + 1,
				"startsong", songInfo->subsongs.def - songInfo->subsongs.min,
				"length", songInfo->duration,
				"title", modname,
				"format", songInfo->playername
			);
			//printf("UADE:%s %s\n", songInfo->playerfname, songInfo->playername);
			valid = true;
		}

		ok = chdir(currdir);

		return valid;

	}



	~UADEPlayer() override {
		//if(valid)
		//	exit_song();
	   uade_stop(state);
	   uade_cleanup_state(state);
	   state = nullptr;
	}

	virtual int getSamples(int16_t *target, int noSamples) override {
		//int rc = get_samples((uint8_t*)target, noSamples * 2);
		ssize_t rc = uade_read(target, noSamples*2, state);
		struct uade_notification nf;
		while(uade_read_notification(&nf, state)) {
			if(nf.type == UADE_NOTIFICATION_SONG_END) {
				LOGD("UADE SONG END: %d %d %d %s", nf.song_end.happy, nf.song_end.stopnow, nf.song_end.subsong, nf.song_end.reason);
				setMeta("song", nf.song_end.subsong+1);
				if(nf.song_end.stopnow)
					musicStopped = true;
			} else if(nf.type == UADE_NOTIFICATION_MESSAGE) {
				LOGD("Amiga message: %s\n", nf.msg);
			} else
				LOGD("Unknown notification: %d\n", nf.type);
			uade_cleanup_notification(&nf);
		}
		//LOGD("RC %d", rc);
		if(rc > 0)
			return rc/2;
		return rc;
	}

	virtual bool seekTo(int song, int seconds) {
		//if(musicStopped) {
		//	if(uade_play(currentFileName.c_str(), -1, state) == 1) {
		//		songInfo = uade_get_song_info(state);
		//		musicStopped = false;
		//	}
		//}
		uade_seek(UADE_SEEK_SUBSONG_RELATIVE, 0, song + songInfo->subsongs.min, state);
		//set_song(song);
		return true;	
	}

private:
	string dataDir;
	bool valid;
	struct uade_state *state;
	const struct uade_song_info *songInfo;
	string baseName;
	string currentFileName;
	bool musicStopped;
};

bool UADEPlugin::canHandle(const std::string &name) {
	return true;
}

ChipPlayer *UADEPlugin::fromFile(const std::string &fileName) {

	auto *player = new UADEPlayer(dataDir + "/data/uade");
	if(!player->load(fileName)) {
		delete player;
		player = nullptr;
	}
	return player;
};

}