#include <rsspp.h>
#include <json.h>
#include <utils.h>
#include <remote_api.h>
#include <newsblur_api.h>
#include <algorithm>
#include <string.h>

namespace newsbeuter {

newsblur_api::newsblur_api(configcontainer * c) : remote_api(c) {
	auth_info = utils::strprintf("username=%s&password=%s", cfg->get_configvalue("newsblur-login").c_str(), cfg->get_configvalue("newsblur-password").c_str());
	api_location = cfg->get_configvalue("newsblur-url");
	num_fetch_pages = (cfg->get_configvalue_as_int("newsblur-num-fetch-articles") + 5) / 6;
	easyhandle = curl_easy_init();
	setup_handle();
}

newsblur_api::~newsblur_api() {
	curl_easy_cleanup(easyhandle);
}

bool newsblur_api::authenticate() {
	json_object * response;
	json_object * status;

	response = newsblur_api::query_api("/api/login", &auth_info);
	status = json_object_object_get(response, "authenticated");
	return json_object_get_boolean(status);
}

std::vector<tagged_feedurl> newsblur_api::get_subscribed_urls() {
	std::vector<tagged_feedurl> result;

	json_object * response = query_api("/reader/feeds", NULL);

	json_object * feeds = json_object_object_get(response, "feeds");

	json_object_iterator it = json_object_iter_begin(feeds);
	json_object_iterator itEnd = json_object_iter_end(feeds);

	while (!json_object_iter_equal(&it, &itEnd)) {
		const char * feed_id = json_object_iter_peek_name(&it);
		json_object * node;
		rsspp::feed current_feed;

		current_feed.rss_version = rsspp::NEWSBLUR_JSON;

		json_object * feed_json = json_object_iter_peek_value(&it);
		node = json_object_object_get(feed_json, "feed_title");
		current_feed.title = json_object_get_string(node);
		node = json_object_object_get(feed_json, "feed_link");
		current_feed.link = json_object_get_string(node);

		known_feeds[feed_id] = current_feed;

		std::vector<std::string> tags = std::vector<std::string>();
		result.push_back(tagged_feedurl(std::string(feed_id), tags));

		json_object_iter_next(&it);
	}

	return result;
}

void newsblur_api::configure_handle(CURL * /*handle*/) {
	// nothing required
}

bool newsblur_api::mark_all_read(const std::string& feed_url) {
	std::string post_data = utils::strprintf("feed_id=%s", feed_url.c_str());
	query_api("/reader/mark_feed_as_read", &post_data);
	return true;
}

bool newsblur_api::mark_article_read(const std::string& guid, bool read) {
	std::string endpoint;
	int separator = guid.find(ID_SEPARATOR);
	std::string feed_id = guid.substr(0, separator);
	std::string article_id = guid.substr(separator + sizeof(ID_SEPARATOR) - 1);

	std::string post_data = "feed_id=" + feed_id + "&" + "story_id=" + article_id;
	LOG(LOG_ERROR, guid.c_str());
	LOG(LOG_ERROR, post_data.c_str());
	if(read) {
		endpoint = "/reader/mark_story_as_read";
	} else {
		endpoint = "/reader/mark_story_as_unread";
	}
	LOG(LOG_ERROR, guid.c_str());

	query_api(endpoint, &post_data);
	return true;
}

bool newsblur_api::update_article_flags(const std::string& oldflags, const std::string& newflags, const std::string& guid) {
	(void)oldflags;
	(void)newflags;
	(void)guid;
	return false;
}

static bool sort_by_pubdate(const rsspp::item& a, const rsspp::item& b) {
	return a.pubDate_ts > b.pubDate_ts;
}

time_t parse_date(const char * raw) {
	struct tm tm;
	memset(&tm, 0, sizeof(tm));

	strptime(raw, "%Y-%m-%d %H:%M:%S", &tm);
	return mktime(&tm);
}

rsspp::feed newsblur_api::fetch_feed(const std::string& id) {
	rsspp::feed f = known_feeds[id];

	for(unsigned int i = 1; i <= num_fetch_pages; i++) {

		std::string page = utils::to_string(i);

		json_object * query_result = query_api("/reader/feed/" + id + "?read_filter=unread&page=" + page, NULL);

		if (!query_result)
			return f;

		json_object * stories = json_object_object_get(query_result, "stories");

		if (!stories)
			return f;

		if (json_object_get_type(stories) != json_type_array) {
			LOG(LOG_ERROR, "newsblur_api::fetch_feed: content is not an array");
			return f;
		}

		struct array_list * items = json_object_get_array(stories);
		int items_size = array_list_length(items);
		LOG(LOG_DEBUG, "newsblur_api::fetch_feed: %d items", items_size);

		for (int i=0;i<items_size;i++) {
			struct json_object * item_obj = (struct json_object *)array_list_get_idx(items, i);
			const char * article_id = json_object_get_string(json_object_object_get(item_obj, "id"));
			const char * title = json_object_get_string(json_object_object_get(item_obj, "story_title"));
			const char * link = json_object_get_string(json_object_object_get(item_obj, "story_permalink"));
			const char * content = json_object_get_string(json_object_object_get(item_obj, "story_content"));
			const char * pub_date = json_object_get_string(json_object_object_get(item_obj, "story_date"));
			bool read_status = json_object_get_int(json_object_object_get(item_obj, "read_status"));

			rsspp::item item;

			if (title)
				item.title = title;

			if (link)
				item.link = link;

			if (content)
				item.content_encoded = content;

			item.guid = id + ID_SEPARATOR + article_id;

			if (read_status == 0) {
				item.labels.push_back("newsblur:unread");
			} else if (read_status == 1) {
				item.labels.push_back("newsblur:read");
			}

			item.pubDate_ts = parse_date(pub_date);
			char rfc822_date[128];
			strftime(rfc822_date, sizeof(rfc822_date), "%a, %d %b %Y %H:%M:%S %z", gmtime(&item.pubDate_ts));
			item.pubDate = rfc822_date;

			f.items.push_back(item);
		}
	}

	std::sort(f.items.begin(), f.items.end(), sort_by_pubdate);
	return f;
}

void newsblur_api::setup_handle(void) {
	utils::set_common_curl_options(easyhandle, cfg);
	// custom
	curl_easy_setopt(easyhandle, CURLOPT_SSL_VERIFYPEER, 1);
	curl_easy_setopt(easyhandle, CURLOPT_COOKIEFILE, "");

}


static size_t my_write_data(void *buffer, size_t size, size_t nmemb, void *userp) {
	std::string * pbuf = static_cast<std::string *>(userp);
	pbuf->append(static_cast<const char *>(buffer), size * nmemb);
	return size * nmemb;
}

json_object * newsblur_api::query_api(const std::string& endpoint, const std::string* postdata) {
	std::string buf;

	handle_lock.lock();

	curl_easy_setopt(easyhandle, CURLOPT_URL, (api_location + endpoint).c_str());
	curl_easy_setopt(easyhandle, CURLOPT_WRITEFUNCTION, my_write_data);
	curl_easy_setopt(easyhandle, CURLOPT_WRITEDATA, &buf);

	if(postdata != NULL) {
		curl_easy_setopt(easyhandle, CURLOPT_POST, 1);
		curl_easy_setopt(easyhandle, CURLOPT_POSTFIELDS, postdata->c_str());
		LOG(LOG_INFO, "newsblur_api::query_api(%s)[%s]: %s", endpoint.c_str(), postdata->c_str(), buf.c_str());
	} else {
		curl_easy_setopt(easyhandle, CURLOPT_POST, 0);
		LOG(LOG_INFO, "newsblur_api::query_api(%s)[-]: %s", endpoint.c_str(), buf.c_str());
	}

	curl_easy_perform(easyhandle);
	handle_lock.unlock();

	return json_tokener_parse(buf.c_str());
}

}
