#define CPPHTTPLIB_OPENSSL_SUPPORT

#include <string>
#include <array>
#include <random>
#include <optional>
#include <regex>
#include <exception>

#include "httplib.h"
#include "openssl/aes.h"

#include "config.h"

using namespace std;
vector<unsigned char> rndstr(size_t len)
{
	static constexpr string_view random_chars = "ABCDEFGHJKMNPQRSTWXYZabcdefhijkmnprstwxyz2345678";
	static default_random_engine e;
	static uniform_int_distribution<size_t> rnd(0u, random_chars.length() - 1);
	vector<unsigned char> ret;
	for (size_t i = 0; i < len; ++i)
		ret.push_back(random_chars[rnd(e)]);
	return ret;
}
void PKCS7Padding(vector<unsigned char> &str)
{
	unsigned char remain = 16 - str.size() % 16;
	for (size_t i = 0; i < remain; ++i)
		str.push_back(remain);
}
vector<unsigned char> addlen(string_view raw_info)
{
	auto in = rndstr(64);
	copy(raw_info.begin(), raw_info.end(), back_inserter(in));
	PKCS7Padding(in);
	return in;
}
string aes_128_encryption(string_view raw_info, string_view aes_key)
{
	AES_KEY enc_key;
	string dec_out;
	if (aes_key.size() != 16)
	{
		throw invalid_argument("aes_key error!");
	}
	AES_set_encrypt_key((unsigned char *)aes_key.data(), 128, &enc_key);
	auto in = addlen(raw_info), iv = rndstr(16);
	dec_out.resize(in.size());
	AES_cbc_encrypt(in.data(), (unsigned char *)dec_out.data(), in.size(), &enc_key, iv.data(), AES_ENCRYPT);
	return httplib::detail::base64_encode(dec_out);
}
auto parseHTML(string_view s, string_view pattern)
{
	auto pos = s.find(pattern);
	if (pos == string::npos || s.begin() + pos + pattern.length() >= s.end())
	{
		throw invalid_argument("can't find lt");
	}
	pos += pattern.length();
	auto pos2 = string_view(&*(s.begin() + pos), (s.size() - pos)).find(R"(")");
	return s.substr(pos, pos2);
}
auto get_lt(string_view s)
{
	return parseHTML(s, R"(<input type="hidden" name="lt" value=")");
}
auto get_aeskey(string_view s)
{
	return parseHTML(s, R"(<input type="hidden" id="pwdDefaultEncryptSalt" value=")");
}
pair<string, string> url_parse(const string_view url)
{
	char pre = '\0';
	for (auto iter = url.begin(); iter != url.end(); ++iter)
	{
		if (*iter == '/' && pre != '/' && (iter + 1 == url.end() || *(iter + 1) != '/'))
		{
			return {string(url.substr(0, iter - url.begin())), string(url.substr(iter - url.begin(), url.end() - iter))};
		}
		pre = *iter;
	}
	return {"", ""};
}
unique_ptr<httplib::Client> login(const string_view src_url, optional<string_view> login_check_path = {})
{
	SPDLOG_INFO("start login");
	auto host = "https://ids.xmu.edu.cn"s;
	auto path = "/authserver/login?service="s.append(src_url);
	auto headers = httplib::Headers{{"User-Agent", "Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36 (KHTML, like Gecko) Chrome/92.0.4515.159 Safari/537.36 Edg/92.0.902.84"}};
    SPDLOG_INFO("start init client");
	httplib::Client cli(host.data());
	cli.set_keep_alive(true);
	cli.set_connection_timeout(200s);
	cli.set_read_timeout(200s);
	cli.set_write_timeout(200s);
	cli.enable_server_certificate_verification(false);
    SPDLOG_INFO("finish init client");
	auto res = cli.Get(path.data(), headers);
	if (!res)
	{
		SPDLOG_INFO("first connect failed...");
		SPDLOG_ERROR("failed to open the XMU Universal login URL:{},error_code:{}",path,to_string(res.error()));
		return {};
	}
	SPDLOG_INFO("first connect ok...");
	SPDLOG_DEBUG("Headers:{}",res.value().headers);
	SPDLOG_DEBUG("Body:{}",res.value().body);
	SPDLOG_DEBUG("Status:{}",res.value().status);
	SPDLOG_DEBUG("Cookie for try to login:{}", res->get_header_value("Set-Cookie"));

	headers.emplace("Cookie", res->get_header_value("Set-Cookie"));
	this_thread::sleep_for(2s);
	auto [username, password] = get_user_info();

	//SPDLOG_DEBUG("Login username:{}",username);
	//SPDLOG_DEBUG("Login password:{}",password);

	auto params = httplib::Params
	{
		{"dllt", "userNamePasswordLogin"},
		{"execution", "e1s1"},
		{"_eventId", "submit"},
		{"rmShown", "1"},
		{"lt", string(get_lt(res->body))},
		{"username", username},
		{"password", aes_128_encryption(password, get_aeskey(res->body))}
	};

	SPDLOG_DEBUG("URL:{}",path);
	//SPDLOG_DEBUG("Headers:{}",headers);
	//SPDLOG_DEBUG("Params:{}",params);

	res = cli.Post(path.data(), headers, params);
	if (!res)
	{
		SPDLOG_ERROR("failed to login:{} ,when post username and password.",to_string(res.error()));
		return {};
	}

	SPDLOG_DEBUG("Headers:{}",res.value().headers);
	SPDLOG_DEBUG("Body:{}",res.value().body);
	SPDLOG_DEBUG("Status:{}",res.value().status);

	auto location=res->get_header_value("Location");
	if (location.empty())
	{
		SPDLOG_ERROR("failed to get the redirect-location");
		return{};
	}
	tie(host, path) = url_parse(res->get_header_value("Location"));
	SPDLOG_DEBUG("redirect host:{} path:{}",host, path);
	cli.stop();

	auto cli2 = make_unique<httplib::Client>(host.data());
	cli2->set_keep_alive(true);
	cli2->set_connection_timeout(200s);
	cli2->set_read_timeout(200s);
	cli2->set_write_timeout(200s);
	cli2->enable_server_certificate_verification(false);
	res = cli2->Get(path.data(), headers);

	if (!res)
	{
		SPDLOG_ERROR("failed to login:{}, when redirect.",to_string(res.error()));
		SPDLOG_DEBUG("Host:{} Path:{}",host,path);
		SPDLOG_DEBUG("Headers:{}",headers);
		return {};
	}

	//SPDLOG_DEBUG("Headers:{}",res.value().headers);
	SPDLOG_DEBUG("Body:{}",res.value().body);
	SPDLOG_DEBUG("Status:{}",res.value().status);

	auto [iter, iterend] = res->headers.equal_range("Set-Cookie");
	for (; iter != iterend; ++iter)
	{
		if (iter->second.find("SAAS_U") != string::npos)
		{
			cli2->set_default_headers({{"User-Agent", get_user_agent()}, {"Cookie", iter->second}});
			break;
		}
	}
	if (iter == iterend)
	{
		SPDLOG_ERROR("Can't found the cookie called SAAS_U for saving the login status!");
		return {};
	}
	
	SPDLOG_INFO("login succeed!");
	//SPDLOG_DEBUG(iter->second);
	
	if (login_check_path.has_value())
	{
		res = cli2->Get(string(login_check_path.value()).data());
		//SPDLOG_DEBUG("Headers:{}",res.value().headers);
		SPDLOG_DEBUG("Body:{}",res.value().body);
		SPDLOG_DEBUG("Status:{}",res.value().status);
		if (!res)
		{
			SPDLOG_ERROR("login check failed:{}!",to_string(res.error()));
			return {};
		}
		if (res->status != 200)
		{
			SPDLOG_ERROR("login check failed!");
			SPDLOG_DEBUG("Headers:{}",res.value().headers);
			SPDLOG_DEBUG("Body:{}",res.value().body);
			SPDLOG_DEBUG("Status:{}",res.value().status);
			return {};
		}
	}

	return cli2;
}