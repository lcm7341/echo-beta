#pragma once
#include <Geode/Geode.hpp>
#include <Geode/utils/web.hpp>
#include <Geode/utils/async.hpp>
#include <array>
#include <string>
#include <vector>

struct OnlineReplay {
	std::string id;
	std::string title;
	std::string notes;
	int level_id = 0;
	std::string level_name;
	float tps = 0.f;
	int inputs = 0;
	std::string physics;
	std::string uploader;
	bool claims_completion = false;
	bool verified = false;
	int downloads = 0;
};

struct OnlineClickpack {
	std::string id;
	std::string name;
	std::string notes;
	std::string uploader;
	std::string types;
	int downloads = 0;
	bool has_releases = false;
};

struct OnlineTheme {
	std::string id;
	std::string name;
	std::string notes;
	std::string uploader;
	int downloads = 0;
	std::vector<std::array<float, 4>> swatches;
};

enum class OnlineKind {
	Macros,
	Clickpacks,
	Themes
};

class ReplayDb {
public:
	static ReplayDb& get();

	bool signed_in();
	std::string username();
	bool signing_in() const { return signing; }
	void sign_in();
	void sign_out();

	void search(OnlineKind kind, const std::string& query, bool verified_only);
	bool searching() const { return looking; }
	void play(const OnlineReplay& replay);
	void save(const OnlineReplay& replay);
	bool downloading() const { return fetching; }
	void upload(const std::string& title, const std::string& notes, bool claims_completion);
	bool open_level(std::string& name, int& id);
	static bool names_match(const std::string& macro_name, const std::string& level_name);
	bool uploading() const { return sending; }
	void request_web_code();
	void open_on_website(const OnlineReplay& replay);
	void open_page(const std::string& path);
	void preview_sound(const OnlineClickpack& pack, const char* type);
	void install_clickpack(const OnlineClickpack& pack, int use_for_player);
	void upload_clickpack(const std::string& folder, const std::string& name, const std::string& notes);
	std::vector<std::string> installed_clickpacks();
	void apply_theme(const OnlineTheme& theme);
	void upload_theme(const std::string& name, const std::string& notes);
	void open_website();
	void open_sign_in_page();
	bool can_go_to_level(const OnlineReplay& replay);
	void go_to_level(const OnlineReplay& replay);
	void copy_level_id(const OnlineReplay& replay);

	std::vector<OnlineReplay> results;
	std::vector<OnlineClickpack> clickpacks;
	std::vector<OnlineTheme> themes;
	int total = 0;
	bool searched = false;
	std::string status;
	bool status_bad = false;
	std::string web_code;
	bool web_code_ready = false;

private:
	void fail(const std::string& text);
	void succeed(const std::string& text);
	bool session_ok(const geode::utils::web::WebResponse& response);
	geode::utils::web::WebRequest request(bool authorized);
	void download(const OnlineReplay& replay, bool keep);

	bool signing = false, looking = false, fetching = false, sending = false;
	geode::async::TaskHolder<geode::Result<std::string>> auth_task;
	void send_file(const std::string& path, const char* meta_header, const std::string& meta, std::vector<unsigned char> bytes);

	geode::async::TaskHolder<geode::utils::web::WebResponse> login_task, search_task, download_task, upload_task, code_task, preview_task;
};
