/*
* Tencent is pleased to support the open source community by making Puerts available.
* Copyright (C) 2020 Tencent.  All rights reserved.
* Puerts is licensed under the BSD 3-Clause License, except for the third-party components listed in the file 'LICENSE' which may be subject to their corresponding license terms.
* This file is subject to the terms and conditions defined in file 'LICENSE', which is part of this source code package.
*/
#include "JSEngine.h"
#include <cstring>
#include "V8Utils.h"
#include "Log.h"
#include "pesapi.h"
#include "CppObjectMapper.h"


#define API_LEVEL 36

using puerts::JSEngine;
using puerts::FResultInfo;
using puerts::FV8Utils;
using puerts::JsValueType;

#ifdef __cplusplus
extern "C" {
#endif

V8_EXPORT int GetV8PapiVersion()
{
    return PESAPI_VERSION;
}

V8_EXPORT pesapi_env_ref GetV8PapiEnvRef(v8::Isolate *Isolate)
{
#ifdef THREAD_SAFE
    v8::Locker Locker(Isolate);
#endif
    v8::Isolate::Scope IsolateScope(Isolate);
    auto jsEnv = FV8Utils::IsolateData<JSEngine>(Isolate);
    v8::HandleScope HandleScope(Isolate);
    v8::Local<v8::Context> Context = jsEnv->BackendEnv.MainContext.Get(Isolate);
    v8::Context::Scope ContextScope(Context);
    
    auto env = reinterpret_cast<pesapi_env>(*Context); //TODO: 实现相关
    return v8impl::g_pesapi_ffi.create_env_ref(env);
}

V8_EXPORT pesapi_ffi* GetV8FFIApi()
{
    return &v8impl::g_pesapi_ffi;
}

V8_EXPORT pesapi_env_ref CreateV8PapiEnvRef()
{
    auto jsEnv = new JSEngine(nullptr, nullptr);
#ifdef THREAD_SAFE
    v8::Locker Locker(jsEnv->MainIsolate);
#endif
    v8::Isolate::Scope IsolateScope(jsEnv->MainIsolate);
    v8::HandleScope HandleScope(jsEnv->MainIsolate);
    v8::Local<v8::Context> Context = jsEnv->BackendEnv.MainContext.Get(jsEnv->MainIsolate);
    v8::Context::Scope ContextScope(Context);
    
    auto env = reinterpret_cast<pesapi_env>(*Context); //TODO: 实现相关
    return v8impl::g_pesapi_ffi.create_env_ref(env);
}

V8_EXPORT void DestroyV8PapiEnvRef(pesapi_env_ref env_ref)
{
    auto scope = v8impl::g_pesapi_ffi.open_scope(env_ref);
    auto env = v8impl::g_pesapi_ffi.get_env_from_ref(env_ref);
    auto context = reinterpret_cast<v8::Context*>(env);
    v8::Isolate *isolate = context->GetIsolate();
    v8impl::g_pesapi_ffi.close_scope(scope);
    auto JsEngine = FV8Utils::IsolateData<JSEngine>(isolate);
    delete JsEngine;
}

V8_EXPORT v8::Isolate *GetV8Isolate(pesapi_env_ref env_ref)
{
    auto scope = v8impl::g_pesapi_ffi.open_scope(env_ref);
    auto env = v8impl::g_pesapi_ffi.get_env_from_ref(env_ref);
    auto context = reinterpret_cast<v8::Context*>(env);
    v8::Isolate *isolate = context->GetIsolate();
    v8impl::g_pesapi_ffi.close_scope(scope);
    return isolate;
}

// 清除指定路径的 ESM 缓存，返回是否移除了缓存条目
V8_EXPORT int ClearV8ModuleCache(v8::Isolate *Isolate, const char *Path)
{
    if (Path == nullptr || Path[0] == '\0')
        return 0;
#ifdef THREAD_SAFE
    v8::Locker Locker(Isolate);
#endif
    v8::Isolate::Scope IsolateScope(Isolate);
    v8::HandleScope HandleScope(Isolate);
    puerts::FBackendEnv *Backend = puerts::FBackendEnv::Get(Isolate);
    v8::Local<v8::Context> Context = Backend->MainContext.Get(Isolate);
    v8::Context::Scope ContextScope(Context);
    return Backend->ClearModuleCache(Isolate, Context, Path) ? 1 : 0;
}

// 开始宿主同步更新的缓存暂存，后续清缓存保留原模块并暂停微任务
V8_EXPORT int BeginV8ReloadCache(v8::Isolate *Isolate)
{
#ifdef THREAD_SAFE
    v8::Locker Locker(Isolate);
#endif
    v8::Isolate::Scope IsolateScope(Isolate);
    return puerts::FBackendEnv::Get(Isolate)->BeginReloadCache() ? 1 : 0;
}

// 按宿主提交结果保留候选或恢复原缓存，然后解除微任务暂停
V8_EXPORT int EndV8ReloadCache(v8::Isolate *Isolate, int Success)
{
#ifdef THREAD_SAFE
    v8::Locker Locker(Isolate);
#endif
    v8::Isolate::Scope IsolateScope(Isolate);
    v8::HandleScope HandleScope(Isolate);
    return puerts::FBackendEnv::Get(Isolate)->EndReloadCache(Success != 0) ? 1 : 0;
}

// 查询指定路径是否已完成 ESM 求值，编译或求值失败均返回零
V8_EXPORT int V8ModuleEvaluated(v8::Isolate *Isolate, const char *Path)
{
    if (Path == nullptr || Path[0] == '\0')
        return 0;
#ifdef THREAD_SAFE
    v8::Locker Locker(Isolate);
#endif
    v8::Isolate::Scope IsolateScope(Isolate);
    v8::HandleScope HandleScope(Isolate);
    puerts::FBackendEnv *Backend = puerts::FBackendEnv::Get(Isolate);
    auto Module = Backend->PathToModuleMap.find(Path);
    return Module != Backend->PathToModuleMap.end() && Module->second.Get(Isolate)->GetStatus() == v8::Module::kEvaluated ? 1 : 0;
}

// 查询当前模块的私有绑定记录，不通过模块 export 暴露内部协议
//
// 获得这份记录，是为了拿到修改旧模块导出变量的入口 set。普通 JS 无法直接给模块 namespace 的导出属性赋值
//
// 这里的“私有绑定记录”，是这轮实现为每个模块生成的一份描述对象，包含：
// - module：模块的 namespace，也就是访问其导出成员的对象
// - names：可以修改的本地导出名称，例如 tick
// - set(name, value)：编译器生成的函数，用来修改模块内部对应的变量
static void CaptureV8Module(const v8::FunctionCallbackInfo<v8::Value>& Info)
{
    v8::Isolate* Isolate = Info.GetIsolate();
    v8::Local<v8::Context> Context = Isolate->GetCurrentContext();
    puerts::FBackendEnv* Backend = puerts::FBackendEnv::Get(Isolate);
    v8::String::Utf8Value Path(Isolate, Info[0]);
    auto Found = Backend->PathToModuleMap.find(*Path);
    if (Found == Backend->PathToModuleMap.end() || Found->second.Get(Isolate)->GetStatus() != v8::Module::kEvaluated)
        return;
    v8::Local<v8::Object> Namespace = Found->second.Get(Isolate)->GetModuleNamespace().As<v8::Object>();
    v8::Local<v8::Value> Record;
    if (Namespace->GetPrivate(Context, v8::Private::ForApi(Isolate,
        v8::String::NewFromUtf8Literal(Isolate, "puerts.module.bindings"))).ToLocal(&Record) && Record->IsObject())
    {
        Info.GetReturnValue().Set(Record);
    }
    else
    {
        v8::Local<v8::Object> Empty = v8::Object::New(Isolate);
        Empty->CreateDataProperty(Context, v8::String::NewFromUtf8Literal(Isolate, "module"), Namespace).Check();
        Info.GetReturnValue().Set(Empty);
    }
}

// 临时交接原生查询函数，Runtime 在执行业务前立即取走并删除全局属性
V8_EXPORT void InstallV8ModuleCapture(v8::Isolate* Isolate)
{
#ifdef THREAD_SAFE
    v8::Locker Locker(Isolate);
#endif
    v8::Isolate::Scope IsolateScope(Isolate);
    v8::HandleScope HandleScope(Isolate);
    v8::Local<v8::Context> Context = puerts::FBackendEnv::Get(Isolate)->MainContext.Get(Isolate);
    v8::Context::Scope ContextScope(Context);
    Context->Global()->CreateDataProperty(Context, v8::String::NewFromUtf8Literal(Isolate, "__puertsModuleCapture"),
        v8::Function::New(Context, CaptureV8Module).ToLocalChecked()).Check();
}

V8_EXPORT void LowMemoryNotification(v8::Isolate *Isolate)
{
    auto JsEngine = FV8Utils::IsolateData<JSEngine>(Isolate);
    JsEngine->LowMemoryNotification();
}
V8_EXPORT bool IdleNotificationDeadline(v8::Isolate *Isolate, double DeadlineInSeconds)
{
    auto JsEngine = FV8Utils::IsolateData<JSEngine>(Isolate);
    return JsEngine->IdleNotificationDeadline(DeadlineInSeconds);
}
V8_EXPORT void RequestMinorGarbageCollectionForTesting(v8::Isolate *Isolate)
{
    auto JsEngine = FV8Utils::IsolateData<JSEngine>(Isolate);
    JsEngine->RequestMinorGarbageCollectionForTesting();
}
V8_EXPORT void RequestFullGarbageCollectionForTesting(v8::Isolate *Isolate)
{
    auto JsEngine = FV8Utils::IsolateData<JSEngine>(Isolate);
    JsEngine->RequestFullGarbageCollectionForTesting();
}

//-------------------------- begin debug --------------------------

V8_EXPORT void CreateInspector(v8::Isolate *Isolate, int32_t Port)
{
    auto JsEngine = FV8Utils::IsolateData<JSEngine>(Isolate);
    JsEngine->CreateInspector(Port);
}

V8_EXPORT void DestroyInspector(v8::Isolate *Isolate)
{
    auto JsEngine = FV8Utils::IsolateData<JSEngine>(Isolate);
    JsEngine->DestroyInspector();
}

V8_EXPORT int InspectorTick(v8::Isolate *Isolate)
{
    auto JsEngine = FV8Utils::IsolateData<JSEngine>(Isolate);
    return JsEngine->InspectorTick() ? 1 : 0;
}

V8_EXPORT void LogicTick(v8::Isolate *Isolate)
{
    auto JsEngine = FV8Utils::IsolateData<JSEngine>(Isolate);
    return JsEngine->LogicTick();
}


V8_EXPORT void TerminateExecution(v8::Isolate *Isolate)
{
    Isolate->TerminateExecution();
}

//-------------------------- end debug --------------------------

#ifdef __cplusplus
}
#endif
