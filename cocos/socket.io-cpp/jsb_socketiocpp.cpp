#include "jsb_socketiocpp.hpp"

#include "cocos/scripting/js-bindings/jswrapper/SeApi.h"
#include "cocos/scripting/js-bindings/manual/jsb_conversions.hpp"
#include "cocos/scripting/js-bindings/manual/jsb_global.h"

#include "src/sio_client.h"
#include "base/ccUTF8.h"
#include "platform/CCApplication.h"
#include "base/CCScheduler.h"

using namespace cocos2d;
using namespace sio;

se::Class* __jsb_SocketIOCPP_class = nullptr;

class JSB_SocketIODelegate;
class SIOClient;


class JSB_SocketIODelegate : public Ref
{
public:
	//c++11 map to callbacks
	typedef std::unordered_map<std::string/* eventName */, se::ValueArray/* 0:callbackFunc, 1:target */> JSB_SIOCallbackRegistry;	

	JSB_SocketIODelegate(SIOClient* client): _client(client) {
	}

	virtual ~JSB_SocketIODelegate()
	{
		CCLOGINFO("In the destructor of JSB_SocketIODelegate(%p)", this);
	}

	void onOpen() {
		CCLOG("Socket open");
		fireEventToScript("connect", sio::null_message::create());
	}

	void onFail() {
		CCLOG("Socket fail");
		fireEventToScript("connect_error", sio::null_message::create());
	}

	void onClose(sio::client::close_reason const& reason)
	{
		CCLOG("JSB SocketIO::SIODelegate->onClose method called from native");
		if (reason == 0) {
			fireEventToScript("disconnect", "close_reason_normal");
		}
		else {
			fireEventToScript("disconnect", "close_reason_drop");
		}

		auto iter = se::NativePtrToObjectMap::find(_client);
		if (iter != se::NativePtrToObjectMap::end())
		{
			iter->second->unroot();
		}

		if (getReferenceCount() == 1)
		{
			autorelease();
		}
		else
		{
			release();
		}
	}

	void onError(const sio::message::ptr data)
	{
		CCLOG("JSB SocketIO::SIODelegate->onError method called from native with ");
		this->fireEventToScript("error", data);

		auto iter = se::NativePtrToObjectMap::find(_client);
		if (iter != se::NativePtrToObjectMap::end())
		{
			iter->second->unroot();
		}
	}

	void onEvent(sio::event& event) {
		auto name = event.get_name();
		CCLOG("Socket on event %s",name.c_str());
		fireEventToScript(name, event.get_message());
	}

	se::Value ioData2seValue(sio::message::ptr const data) {
		se::Value dataVal;
		if (data != nullptr) {
			switch (data->get_flag())
			{
			case sio::message::flag_null:
				dataVal.setNull(); break;
			case sio::message::flag_integer:
				dataVal.setNumber(data->get_int()); break;
			case sio::message::flag_double:
				dataVal.setNumber(data->get_double()); break;
			case sio::message::flag_string:
				dataVal.setString(data->get_string()); break;
			case sio::message::flag_boolean:
				dataVal.setBoolean(data->get_bool()); break;
			case sio::message::flag_binary: {
				auto const bin = data->get_binary();
				se::HandleObject dataObj(se::Object::createArrayBufferObject((void*)bin->c_str(), bin->length()));
				dataVal = se::Value(dataObj);
				break;
			}
			default:
				CCLOGWARN("Not support data type, set to null");
				dataVal.setNull();
				break;
			}
		}
		else {
			dataVal.setNull();
		}
		return dataVal;
	}

	void fireEventToScript(const std::string& eventName, std::string data) {
		fireEventToScript(eventName, sio::string_message::create(data));
	}

	void fireEventToScript(const std::string& eventName, sio::message::ptr const& data)
	{
		// script engine need to run on main thread
		cocos2d::Application::getInstance()->getScheduler()->performFunctionInCocosThread([eventName, data, this] {
			CCLOG("JSB SocketIO::SIODelegate->fireEventToScript method called from native with name '%s'", eventName.c_str());

			se::ScriptEngine::getInstance()->clearException();
			se::AutoHandleScope hs;

			if (cocos2d::Application::getInstance() == nullptr)
				return;

			auto iter = se::NativePtrToObjectMap::find(_client); //IDEA: client probably be a new value with the same address as the old one, it may cause undefined result.
			if (iter == se::NativePtrToObjectMap::end())
				return;

			se::Value dataVal = ioData2seValue(data);

			JSB_SIOCallbackRegistry::iterator it = _eventRegistry.find(eventName);

			if (it != _eventRegistry.end())
			{
				const se::ValueArray& cbStruct = it->second;
				assert(cbStruct.size() == 2);
				const se::Value& callback = cbStruct[0];
				const se::Value& target = cbStruct[1];
				if (callback.isObject() && callback.toObject()->isFunction() && target.isObject())
				{
					se::ValueArray args;
					args.push_back(dataVal);
					callback.toObject()->call(args, target.toObject());
				}
			}
		});		
	}

	void addEvent(const std::string& eventName, const se::Value& callback, const se::Value& target)
	{
		assert(callback.isObject() && callback.toObject()->isFunction());
		assert(target.isObject());
		_eventRegistry[eventName].clear();
		_eventRegistry[eventName].push_back(callback);
		_eventRegistry[eventName].push_back(target);
		target.toObject()->attachObject(callback.toObject());
	}

private:
	JSB_SIOCallbackRegistry _eventRegistry;
	SIOClient* _client;
};

// wrapper class use for cocos
class SIOClient : public Ref {

public:
	SIOClient() {
		_client = new sio::client();
		_delegate = new JSB_SocketIODelegate(this);
		_delegate->retain();

		_client->set_reconnect_attempts(0);
		_client->set_open_listener(std::bind(&JSB_SocketIODelegate::onOpen, _delegate));
		_client->set_close_listener(std::bind(&JSB_SocketIODelegate::onClose, _delegate, std::placeholders::_1));
		_client->set_fail_listener(std::bind(&JSB_SocketIODelegate::onFail, _delegate));
		_client->socket()->on_error(std::bind(&JSB_SocketIODelegate::onError, _delegate, std::placeholders::_1));
	}

	virtual ~SIOClient() {
		_client->sync_close();
		_client->clear_con_listeners();
		_delegate->release();
		delete _client;
	}

public:
	JSB_SocketIODelegate* delegate() { return _delegate; }
	sio::client* client() { return _client; }

private:
	JSB_SocketIODelegate* _delegate = nullptr;
	sio::client* _client = nullptr;

};


static bool SocketIO_finalize(se::State& s)
{
	SIOClient* cobj = (SIOClient*)s.nativeThisObject();
	CCLOGINFO("jsbindings: finalizing JS object %p (SocketIO)", cobj);
	JSB_SocketIODelegate* delegate = cobj->delegate();
	if (delegate->getReferenceCount() == 1)
	{
		delegate->autorelease();
	}
	else
	{
		delegate->release();
	}
	cobj->release();
	return true;
}
SE_BIND_FINALIZE_FUNC(SocketIO_finalize)

static bool SocketIO_emit(se::State& s)
{
	const auto& args = s.args();
	int argc = (int)args.size();
	SIOClient* cobj = (SIOClient*)s.nativeThisObject();

	if (argc >= 1)
	{
		bool ok = false;
		std::string eventName;
		ok = seval_to_std_string(args[0], &eventName);
		SE_PRECONDITION2(ok, false, "Converting eventName failed!");

		sio::message::list payload;
		se::Object* ackFunc = nullptr;
		if (argc >= 2)
		{
			for (int i = 1; i < argc; i++) {
				const auto& arg = args[i];
				if (arg.isNullOrUndefined()) {
					payload.push(sio::null_message::create());
				}
				else if (arg.isBoolean()) {
					bool b;
					ok = seval_to_boolean(arg, &b);
					SE_PRECONDITION2(ok, false, "Converting payload failed!");
					payload.push(sio::bool_message::create(b));
				}
				else if (arg.isNumber()) {
					float f;
					ok = seval_to_float(arg, &f);
					SE_PRECONDITION2(ok, false, "Converting payload failed!");
					payload.push(sio::double_message::create(f));
				}
				else if (arg.isString()) {
					std::string s;
					ok = seval_to_std_string(arg, &s);
					SE_PRECONDITION2(ok, false, "Converting payload failed!");
					payload.push(sio::string_message::create(s));
				}
				else if (arg.isObject()) {
					se::Object* dataObj = arg.toObject();
					uint8_t* ptr = nullptr;
					size_t length = 0;
					if (dataObj->isArrayBuffer())
					{
						ok = dataObj->getArrayBufferData(&ptr, &length);
						SE_PRECONDITION2(ok, false, "getArrayBufferData failed!");
						payload.push(sio::binary_message::create(std::shared_ptr<const std::string>(new std::string((char*)ptr, length))));
					}
					else if (dataObj->isTypedArray())
					{
						ok = dataObj->getTypedArrayData(&ptr, &length);
						SE_PRECONDITION2(ok, false, "getTypedArrayData failed!");
						payload.push(sio::binary_message::create(std::shared_ptr<const std::string>(new std::string((char*)ptr, length))));
					}
					else if (dataObj->isFunction()) {		// ack callback
						if (i == argc - 1) {
							ackFunc = dataObj;
						}
						else {
							SE_REPORT_ERROR("ACK function should end of param list");
						}
					}
					
				}
				else {
					SE_REPORT_ERROR("Not support arg value type");
				}
			}
		}


		std::function<void(sio::message::list const&)> ack = nullptr;
		if (ackFunc != nullptr) {
			auto target = se::Value(s.thisObject());
			auto callback = se::Value(ackFunc);
			ack = [target, cobj, callback](sio::message::list const& msgs) {
				cocos2d::Application::getInstance()->getScheduler()->performFunctionInCocosThread([target, cobj, callback, msgs] {
					se::ScriptEngine::getInstance()->clearException();
					se::AutoHandleScope hs;

					if (cocos2d::Application::getInstance() == nullptr)
						return;

					se::ValueArray args;
					for (int i = 0; i < msgs.size(); i++) {
						args.push_back(cobj->delegate()->ioData2seValue(msgs[i]));
					}
					callback.toObject()->call(args, target.toObject());
				});				
			};
		}

		cobj->client()->socket()->emit(eventName, payload, ack);
		return true;
	}

	SE_REPORT_ERROR("Wrong number of arguments: %d, expected: >=%d", argc, 2);
	return false;
}
SE_BIND_FUNC(SocketIO_emit)

static bool SocketIO_disconnect(se::State& s)
{
	SIOClient* cobj = (SIOClient*)s.nativeThisObject();

	cobj->client()->sync_close();
	return true;

}
SE_BIND_FUNC(SocketIO_disconnect)

static bool SocketIO_on(se::State& s)
{
	const auto& args = s.args();
	int argc = (int)args.size();
	SIOClient* cobj = (SIOClient*)s.nativeThisObject();

	if (argc == 2)
	{
		bool ok = false;
		std::string eventName;
		ok = seval_to_std_string(args[0], &eventName);
		SE_PRECONDITION2(ok, false, "Converting eventName failed!");

		CCLOG("JSB SocketIO register eventName to: '%s'", eventName.c_str());

		cobj->delegate()->addEvent(eventName, args[1], se::Value(s.thisObject()));
		cobj->client()->socket()->on(eventName, sio::socket::event_listener(std::bind(&JSB_SocketIODelegate::onEvent, cobj->delegate(), std::placeholders::_1)));
		return true;
	}

	SE_REPORT_ERROR("Wrong number of arguments: %d, expected: %d", argc, 2);
	return false;
}
SE_BIND_FUNC(SocketIO_on)

// static
static bool SocketIO_connect(se::State& s)
{
	const auto& args = s.args();
	int argc = (int)args.size();
	CCLOG("JSB SocketIO.connect method called");

	se::ScriptEngine::getInstance()->clearException();
	se::AutoHandleScope hs;

	if (argc >= 1 && argc <= 2)
	{
		std::string url;
		std::map<std::string, std::string> query;
		bool ok = false;

		ok = seval_to_std_string(args[0], &url);
		SE_PRECONDITION2(ok, false, "Error processing arguments");
		CCLOG("SocketIO connect to [%s]", url.c_str());

		if (argc == 2)
		{
			if (args[1].isObject())
			{
				// check query option
				std::vector<std::string> allKeys;
				auto dataObj = args[1].toObject();
				if (dataObj->getAllKeys(&allKeys)) {
					se::Value tmp;
					for (const auto& key : allKeys) {
						if (key == "query") {
							if (dataObj->getProperty(key.c_str(), &tmp)) {
								if (tmp.isObject()) {
									seval_to_std_map_string_string(tmp, &query);
								}
							}
						}
					}
				}
			}
		}

		CCLOG("Calling native SocketIO.connect method");
		SIOClient* sioc = new SIOClient();
		sioc->client()->connect(url, query);
		sioc->retain();

		se::Object* obj = se::Object::createObjectWithClass(__jsb_SocketIOCPP_class);
		obj->setPrivateData(sioc);

		s.rval().setObject(obj);
		obj->root();

		return true;
	}
	SE_REPORT_ERROR("JSB SocketIO.connect: Wrong number of arguments");
	return false;
}
SE_BIND_FUNC(SocketIO_connect)


bool register_all_socketiocpp(se::Object* global)
{
	se::Class* cls = se::Class::create("SocketIO", global, nullptr, nullptr);
	cls->defineFinalizeFunction(_SE(SocketIO_finalize));

	cls->defineFunction("emit", _SE(SocketIO_emit));
	cls->defineFunction("disconnect", _SE(SocketIO_disconnect));
	cls->defineFunction("on", _SE(SocketIO_on));

	cls->install();

	//JSBClassType::registerClass<SIOClient>(cls);

	se::Value ctorVal;
	global->getProperty("SocketIO", &ctorVal);
	ctorVal.toObject()->defineFunction("connect", _SE(SocketIO_connect));

	__jsb_SocketIOCPP_class = cls;

	se::ScriptEngine::getInstance()->clearException();
	return true;
}