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

class SocketIO {
public:
	SocketIO() {
		CCLOG("SocketIO constructor");
	}
};

// wrapper over sio:client
class SIOClient : public sio::client, public Ref {

public:
	SIOClient(JSB_SocketIODelegate& delegate) : _delegate(&delegate) {
	}

public:
	JSB_SocketIODelegate* getDelegate() { return _delegate; }
private:
	JSB_SocketIODelegate* _delegate = nullptr;
};


class JSB_SocketIODelegate : public Ref
{
public:
	//c++11 map to callbacks
	typedef std::unordered_map<std::string/* eventName */, se::ValueArray/* 0:callbackFunc, 1:target */> JSB_SIOCallbackRegistry;

	SIOClient* client;

	JSB_SocketIODelegate() {
	}

	virtual ~JSB_SocketIODelegate()
	{
		CCLOGINFO("In the destructor of JSB_SocketIODelegate(%p)", this);
	}

	void onOpen() {
		CCLOG("Socket open");
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

		auto iter = se::NativePtrToObjectMap::find(client);
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

		auto iter = se::NativePtrToObjectMap::find(client);
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

			auto iter = se::NativePtrToObjectMap::find(client); //IDEA: client probably be a new value with the same address as the old one, it may cause undefined result.
			if (iter == se::NativePtrToObjectMap::end())
				return;

			se::Value dataVal;
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
};


static bool SocketIO_finalize(se::State& s)
{
	SIOClient* cobj = (SIOClient*)s.nativeThisObject();
	CCLOGINFO("jsbindings: finalizing JS object %p (SocketIO)", cobj);
	cobj->sync_close();
	cobj->clear_con_listeners();
	JSB_SocketIODelegate* delegate = static_cast<JSB_SocketIODelegate*>(cobj->getDelegate());
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


//static bool SocketIO_prop_getTag(se::State& s)
//{
//	SIOClient* cobj = (SIOClient*)s.nativeThisObject();
//	s.rval().setString(cobj->getTag());
//	return true;
//}
//SE_BIND_PROP_GET(SocketIO_prop_getTag)

//static bool SocketIO_prop_setTag(se::State& s)
//{
//	SIOClient* cobj = (SIOClient*)s.nativeThisObject();
//	cobj->setTag(s.args()[0].toString().c_str());
//	return true;
//}
//SE_BIND_PROP_SET(SocketIO_prop_setTag)

//static bool SocketIO_send(se::State& s)
//{
//	const auto& args = s.args();
//	int argc = (int)args.size();
//	SIOClient* cobj = (SIOClient*)s.nativeThisObject();
//
//	if (argc == 1)
//	{
//		std::string payload;
//		bool ok = seval_to_std_string(args[0], &payload);
//		SE_PRECONDITION2(ok, false, "Converting payload failed!");
//
//		cobj->send(payload);
//		return true;
//	}
//
//	SE_REPORT_ERROR("Not support send function. Use emit('message', ...) instead");
//	return false;
//}
//SE_BIND_FUNC(SocketIO_send)

static bool SocketIO_emit(se::State& s)
{
	const auto& args = s.args();
	int argc = (int)args.size();
	SIOClient* cobj = (SIOClient*)s.nativeThisObject();
	sio::socket::ptr socket = cobj->socket();

	if (argc >= 1)
	{
		bool ok = false;
		std::string eventName;
		ok = seval_to_std_string(args[0], &eventName);
		SE_PRECONDITION2(ok, false, "Converting eventName failed!");

		sio::message::list payload;
		std::function<void(sio::message::list const&)> ack = nullptr;
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
				else {
					SE_REPORT_ERROR("Not support arg value type");
				}
			}
		}

		socket->emit(eventName, payload);
		return true;
	}

	SE_REPORT_ERROR("Wrong number of arguments: %d, expected: >=%d", argc, 2);
	return false;
}
SE_BIND_FUNC(SocketIO_emit)

static bool SocketIO_disconnect(se::State& s)
{
	const auto& args = s.args();
	int argc = (int)args.size();
	SIOClient* cobj = (SIOClient*)s.nativeThisObject();

	if (argc == 0)
	{
		cobj->sync_close();
		return true;
	}

	SE_REPORT_ERROR("Wrong number of arguments: %d, expected: %d", argc, 0);
	return false;
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

		CCLOG("JSB SocketIO eventName to: '%s'", eventName.c_str());

		auto delegate = (JSB_SocketIODelegate *)cobj->getDelegate();
		delegate->addEvent(eventName, args[1], se::Value(s.thisObject()));
		cobj->socket()->on(eventName, sio::socket::event_listener(std::bind(&JSB_SocketIODelegate::onEvent, delegate, std::placeholders::_1)));
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

		JSB_SocketIODelegate* siodelegate = new (std::nothrow) JSB_SocketIODelegate();

		CCLOG("Calling native SocketIO.connect method");
		SIOClient* ret = new SIOClient(*siodelegate);
		if (ret != nullptr)
		{
			ret->set_reconnect_attempts(3);
			ret->set_open_listener(std::bind(&JSB_SocketIODelegate::onOpen, siodelegate));
			ret->set_close_listener(std::bind(&JSB_SocketIODelegate::onClose, siodelegate, std::placeholders::_1));
			ret->set_fail_listener(std::bind(&JSB_SocketIODelegate::onFail, siodelegate));
			ret->connect(url, query);
			ret->socket()->on_error(std::bind(&JSB_SocketIODelegate::onError, siodelegate, std::placeholders::_1));

			ret->retain();
			siodelegate->retain();
			siodelegate->client = ret;

			se::Object* obj = se::Object::createObjectWithClass(__jsb_SocketIOCPP_class);
			obj->setPrivateData(ret);

			s.rval().setObject(obj);
			obj->root();

			return true;
		}
		else
		{
			siodelegate->release();
			SE_REPORT_ERROR("SocketIO.connect return nullptr!");
			return false;
		}
	}
	SE_REPORT_ERROR("JSB SocketIO.connect: Wrong number of arguments");
	return false;
}
SE_BIND_FUNC(SocketIO_connect)

// static
//static bool SocketIO_close(se::State& s)
//{
//	return SocketIO_disconnect(s);
//}
//SE_BIND_FUNC(SocketIO_close)

bool register_all_socketiocpp(se::Object* obj)
{
	se::Class* cls = se::Class::create("SocketIO", obj, nullptr, nullptr);
	cls->defineFinalizeFunction(_SE(SocketIO_finalize));

	//cls->defineProperty("tag", _SE(SocketIO_prop_getTag), _SE(SocketIO_prop_setTag));

	//cls->defineFunction("send", _SE(SocketIO_send));
	cls->defineFunction("emit", _SE(SocketIO_emit));
	cls->defineFunction("disconnect", _SE(SocketIO_disconnect));
	cls->defineFunction("on", _SE(SocketIO_on));

	cls->install();

	JSBClassType::registerClass<SocketIO>(cls);

	se::Value ctorVal;
	obj->getProperty("SocketIO", &ctorVal);
	ctorVal.toObject()->defineFunction("connect", _SE(SocketIO_connect));
	//ctorVal.toObject()->defineFunction("close", _SE(SocketIO_close));

	__jsb_SocketIOCPP_class = cls;

	se::ScriptEngine::getInstance()->clearException();
	return true;
}