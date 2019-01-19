#include "pch.h"
#include "base.h"
#include "helpers.h"

#include "ffi.h"

#include <memory>

namespace meta = xlang::meta::reader;

using namespace fooxlang;

struct writer : xlang::text::writer_base<writer>
{
};

struct separator
{
    writer& w;
    std::string_view _separator{ ", " };
    bool first{ true };

    void operator()()
    {
        if (first)
        {
            first = false;
        }
        else
        {
            w.write(_separator);
        }
    }
};


struct winrt_ns
{
    std::map<std::string_view, winrt_ns> sub_namespaces{};
    meta::cache::namespace_members members{};
};

meta::cache::namespace_members const* find_ns(std::map<std::string_view, winrt_ns> const& namespaces, std::string_view const& ns) noexcept
{
    auto dot_pos = ns.find('.', 0);
    if (dot_pos == std::string_view::npos)
    {
        auto it = namespaces.find(ns);
        if (it == namespaces.end())
        {
            return nullptr;
        }
        else
        {
            return &(it->second.members);
        }
    }
    else
    {
        auto it = namespaces.find(ns.substr(0, dot_pos));
        if (it == namespaces.end())
        {
            return nullptr;
        }
        else
        {
            return find_ns(it->second.sub_namespaces, ns.substr(dot_pos + 1));
        }
    }
}

meta::cache::namespace_members const& get_ns(std::map<std::string_view, winrt_ns> const& namespaces, std::string_view const& ns)
{
    auto _ns = find_ns(namespaces, ns);
    if (_ns == nullptr)
    {
        throw std::out_of_range{ std::string{ns} };
    }

    return *_ns;
}

auto get_system_metadata()
{
    namespace fs = std::experimental::filesystem;

#ifdef _WIN64
    auto sys32 = "c:\\Windows\\System32";
#else
    auto sys32 = "c:\\Windows\\Sysnative";
#endif

    auto winmd = fs::path{ sys32 } / "WinMetadata";

    std::vector<std::string> files;
    for (auto& p : fs::directory_iterator(winmd))
    {
        files.push_back(p.path().string());
    }

    return std::move(files);
}

auto get_namespace_map(meta::cache const& c)
{
    std::map<std::string_view, winrt_ns> root_namespaces;

    for (auto&&[ns, members] : c.namespaces())
    {
        winrt_ns* curns{ nullptr };
        std::map<std::string_view, winrt_ns>* nsmap = &root_namespaces;

        for (auto&& s : get_dotted_name_segments(ns))
        {
            winrt_ns& q = (*nsmap)[s];
            curns = &q;
            nsmap = &(q.sub_namespaces);
        }

        curns->members = members;
    }

    return std::move(root_namespaces);
}

auto get_guid(meta::CustomAttribute const& attrib)
{
    std::vector<meta::FixedArgSig> z = attrib.Value().FixedArgs();

    auto getarg = [&z](std::size_t index) { return std::get<meta::ElemSig>(z[index].value).value; };
    return winrt::guid{
        std::get<uint32_t>(getarg(0)),
        std::get<uint16_t>(getarg(1)),
        std::get<uint16_t>(getarg(2)),
        {
            std::get<uint8_t>(getarg(3)),
            std::get<uint8_t>(getarg(4)),
            std::get<uint8_t>(getarg(5)),
            std::get<uint8_t>(getarg(6)),
            std::get<uint8_t>(getarg(7)),
            std::get<uint8_t>(getarg(8)),
            std::get<uint8_t>(getarg(9)),
            std::get<uint8_t>(getarg(10))
        }
    };
}

auto get_guid(meta::TypeDef const& type)
{
    return get_guid(meta::get_attribute(type, "Windows.Foundation.Metadata", "GuidAttribute"));
}

auto get_arg_types(meta::MethodDef const& method)
{
    method_signature signature{ method };

    std::vector<ffi_type*> arg_types{};

    // explicit this pointer 
    arg_types.push_back(&ffi_type_pointer);

    if (signature.has_params())
    {
        xlang::throw_invalid("not implemented");
    }

    if (signature.return_signature())
    {
        if (signature.return_signature().Type().is_szarray())
        {
            xlang::throw_invalid("not implemented");
        }

        // return values are always pointers
        arg_types.push_back(&ffi_type_pointer);
    }

    return std::move(arg_types);
}

std::vector<ffi_type const*> get_method_ffi_types(meta::MethodDef const& method)
{
    std::vector<ffi_type const*> arg_types{ &ffi_type_pointer };

    method_signature signature{ method };
    if (signature.has_params())
    {
        xlang::throw_invalid("not implemented");
    }

    if (signature.return_signature())
    {
        if (signature.return_signature().Type().is_szarray())
        {
            xlang::throw_invalid("not implemented");
        }

        arg_types.push_back(&ffi_type_pointer);
    }

    return std::move(arg_types);
}

namespace std
{
    template<> struct hash<std::vector<ffi_type const*>>
    {
        std::size_t operator()(std::vector<ffi_type const*> const& vec) const noexcept
        {
            std::hash<ffi_type const*> hasher;
            
            std::size_t seed = vec.size();
            for (auto&& t : vec)
            {
                seed ^= hasher(t) + 0x9e3779b9 + (seed << 6) + (seed >> 2);
            }

            return seed;
        }
    };
}

std::unordered_map<std::vector<ffi_type const*>, std::pair<ffi_cif, std::vector<ffi_type const*>>> cif_cache{};

ffi_cif* get_cif(std::vector<ffi_type const*> const& arg_types)
{
    if (cif_cache.find(arg_types) == cif_cache.end())
    {
        // Make a copy of arg_types for cached ffi_cif struct to point to 
        std::vector<ffi_type const*> arg_types_copy{ arg_types.begin(), arg_types.end() };
        auto pair = std::make_pair<ffi_cif, std::vector<ffi_type const*>>(ffi_cif{}, std::move(arg_types_copy));

        auto ffi_return = ffi_prep_cif(&pair.first, FFI_STDCALL, pair.second.size(), &ffi_type_sint32, const_cast<ffi_type**>(pair.second.data()));
        if (ffi_return != FFI_OK) { xlang::throw_invalid("ffi_prep_cif failure"); }

        cif_cache.emplace(arg_types, std::move(pair));
    }

    return &(cif_cache.at(arg_types).first);
}

void invoke(ffi_cif* cif, winrt::Windows::Foundation::IInspectable const& instance, int offset, std::vector<void*> const& parameters)
{
    winrt::hresult hr;
    std::vector<void*> args{};

    auto vtable = winrt::get_abi(instance);

    args.push_back(&vtable);
    for (auto&& p : parameters)
    {
        args.push_back(const_cast<void**>(&p));
    }

    ffi_call(cif, FFI_FN((*((void***)vtable))[offset]), &hr, args.data());
    winrt::check_hresult(hr);
}

auto get_activation_factory(meta::TypeDef const& type)
{
    // TODO: need a version of get_activation_factory that takes IID as param
    // TODO: need a factory caching scheme that doesn't use compile time type name

    winrt::hstring type_name = winrt::to_hstring(std::string{ type.TypeNamespace() } +"." + std::string{ type.TypeName() });
    return winrt::get_activation_factory(type_name);
}

void write_type_name(writer& w, meta::coded_index<meta::TypeDefOrRef> const& tdrs)
{
    struct type_name_handler : signature_handler_base<type_name_handler>
    {
        using signature_handler_base<type_name_handler>::handle;
        writer& w;

        type_name_handler(writer& w_) : w(w_) {}

        void handle(xlang::meta::reader::TypeDef const& type)
        {
            w.write("%.%", type.TypeNamespace(), type.TypeName());
        }

        void handle(xlang::meta::reader::ElementType type)
        {
            switch (type)
            {
            case xlang::meta::reader::ElementType::Boolean:
                w.write("Boolean"); break;
            case xlang::meta::reader::ElementType::Char:
                w.write("Char"); break;
            case xlang::meta::reader::ElementType::I1:
                w.write("I1"); break;
            case xlang::meta::reader::ElementType::U1:
                w.write("U1"); break;
            case xlang::meta::reader::ElementType::I2:
                w.write("I2"); break;
            case xlang::meta::reader::ElementType::U2:
                w.write("U2"); break;
            case xlang::meta::reader::ElementType::I4:
                w.write("I4"); break;
            case xlang::meta::reader::ElementType::U4:
                w.write("U4"); break;
            case xlang::meta::reader::ElementType::I8:
                w.write("I8"); break;
            case xlang::meta::reader::ElementType::U8:
                w.write("U8"); break;
            case xlang::meta::reader::ElementType::R4:
                w.write("R4"); break;
            case xlang::meta::reader::ElementType::R8:
                w.write("R8"); break;
            case xlang::meta::reader::ElementType::String:
                w.write("String"); break;
            case xlang::meta::reader::ElementType::Object:
                w.write("Object"); break;
            default:
                xlang::throw_invalid("element type not supported");
            }
        }

        void handle(xlang::meta::reader::GenericTypeInstSig const& type)
        {
            handle(type.GenericType());

            w.write("<");
            separator s{ w };
            for (auto&& arg : type.GenericArgs())
            {
                s();
                handle(arg);
            }
            w.write(">");
        }
    };

    type_name_handler tnh{ w };
    tnh.handle(tdrs);
}

int main(int const /*argc*/, char** /*argv*/)
{
    meta::cache c{ get_system_metadata() };
    auto namespaces = get_namespace_map(c);

    meta::TypeDef td_json_object = get_ns(namespaces, "Windows.Data.Json").types.at("JsonObject");

    writer w;

    for (auto&& ii : td_json_object.InterfaceImpl())
    {
        write_type_name(w, ii.Interface());
        w.write("\n");
    }

    w.flush_to_console();

    winrt::init_apartment();
    auto factory = get_activation_factory(td_json_object);

    winrt::Windows::Foundation::IInspectable instance;
    {
        std::vector<ffi_type const*> arg_types{ &ffi_type_pointer, &ffi_type_pointer };
        std::vector<void*> args{ winrt::put_abi(instance) };
        invoke(get_cif(arg_types), factory, 6, args);
    };

    winrt::hstring istringable_str;
    {
        meta::TypeDef td_istringable = get_ns(namespaces, "Windows.Foundation").types.at("IStringable");
        winrt::Windows::Foundation::IInspectable istringable;
        winrt::check_hresult(instance.as(get_guid(td_istringable), winrt::put_abi(istringable)));

        auto arg_types = get_method_ffi_types(td_istringable.MethodList().first[0]);
        std::vector<void*> args{ winrt::put_abi(istringable_str) };
        invoke(get_cif(arg_types), istringable, 6, args);
    }
    printf("%S\n", istringable_str.c_str());

    winrt::hstring ijsonvalue_str;
    {
        meta::TypeDef td_ijsonvalue = get_ns(namespaces, "Windows.Data.Json").types.at("IJsonValue");
        winrt::Windows::Foundation::IInspectable ijsonvalue;
        winrt::check_hresult(instance.as(get_guid(td_ijsonvalue), winrt::put_abi(ijsonvalue)));

        auto arg_types = get_method_ffi_types(td_ijsonvalue.MethodList().first[1]);

        std::vector<void*> args{ winrt::put_abi(ijsonvalue_str) };
        invoke(get_cif(arg_types), ijsonvalue, 7, args);
    };
    printf("%S\n", ijsonvalue_str.c_str());

    return 0;
}