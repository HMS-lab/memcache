# C/C++ Coding Conventions

## Highest Priority: Magic Numbers

- Do not use numeric literals directly in code, except for conventional values such as `0` and `-1` (used for invalid values or error codes).
- Extract every number with an independent semantic meaning into a named constant or enumeration value.
- This rule also applies to tests, especially ports, retry counts, wait counts, timeouts, multipliers, and thresholds.
- Check this rule first when modifying or adding code. Compliance with other naming rules does not excuse magic numbers.

Examples:

```cpp
if (retryCount > 3) { ... }        // wrong: magic number
int timeoutMs = 5000;              // wrong: magic number

constexpr int kMaxRetryCount = 3;
if (retryCount > kMaxRetryCount) { ... }  // correct

constexpr int kDefaultTimeoutMs = 5000;
int timeoutMs = kDefaultTimeoutMs;        // correct
```

## Scope and General Principles

- Use only one naming style for the same concept at the same level.
- New code must follow these conventions.
- Names should express business meaning; avoid meaningless abbreviations.
- Do not introduce a new mixed naming style, except when required for compatibility with an existing public interface.

## Quick Reference

| Entity | Naming Rule | Example |
| --- | --- | --- |
| File | `snake_case` | `mmc_types.h` |
| include guard | `UPPER_SNAKE_CASE` | `MEM_FABRIC_MMC_TYPES_H` |
| namespace | all lowercase | `ock::mmc` |
| Type | `UpperCamelCase` | `MmcMetaManager` |
| C++ function | `UpperCamelCase` | `LoadFromFile` |
| C API function | `snake_case` | `mmc_init` |
| Parameter / local variable | `lowerCamelCase` | `defaultTtl` |
| Public member variable | `lowerCamelCase` | `defaultTtlMs` |
| Private / protected member variable | `lowerCamelCase_` | `defaultTtlMs_` |
| Global variable | `g` + `UpperCamelCase` | `gLogger` |
| Non-local constant | `k` + `UpperCamelCase` | `kDefaultLimit` |
| Local constant | `lowerCamelCase` | `retryCount` |
| Macro / enumeration value | `UPPER_SNAKE_CASE` | `MMC_OK` |
| Magic number | Extract values with independent semantic meaning into named constants or enumerations | `kHttpTestRetryCount` |
| Test file | `*_test.cpp` | `mmc_configuration_test.cpp` |
| Mock file | `mock_*.cpp` | `mock_smem.cpp` |

## Files and Translation Units

### File Naming

- Use `snake_case` for filenames.
- Use only the `.h`, `.c`, and `.cpp` extensions.
- When a C++ file primarily contains one class, name it after the class's meaning and convert the `UpperCamelCase` type name to a `snake_case` filename.

Examples:

- `mmc_types.h`
- `mmc_configuration.cpp`
- `test_meta_manager.cpp`
- `mock_dfc_api.cpp`

### Header Guards

- Use include guards in all headers.
- Name guard macros in `UPPER_SNAKE_CASE`.
- Derive guard macro names from the module path and filename.
- Do not use reserved identifier styles.
- Existing code with double-underscore names (such as `__MEMFABRIC_MMC_DEF_H__`) does not need to be migrated immediately. New files must follow the new style.

Examples:

```c
#ifndef MEM_FABRIC_MMC_TYPES_H
#define MEM_FABRIC_MMC_TYPES_H

/* declarations */

#endif
```

### Namespace Naming

- Use lowercase for namespace names.
- Organize nested namespaces by module hierarchy.
- Use concise, stable business terms for namespace names.
- Use lowercase or `lower_snake_case` for namespace aliases.
- Name inline namespaces according to the namespace rules.

Examples:

- `ock`
- `ock::mmc`
- `ock::mf`
- `shm`
- `namespace mmc_detail = ock::mmc::detail`

## Naming Program Entities

### Type Names

- Use `UpperCamelCase` for `class`, `struct`, `union`, `enum`, `enum class`, `typedef`, and `using` type aliases.
- Name nested types, local types, and named fields associated with anonymous `struct` / `union` declarations according to the type and member-variable rules, respectively.

Examples:

- `Configuration`
- `MmcMetaManager`
- `MmcLocation`
- `EvictResult`
- `MmcUbsIoProxyPtr`
- `PayloadUnion`

### C++ Functions

- Use `UpperCamelCase` for member, free, and static functions.
- Use the class name required by C++ syntax for constructors and destructors.
- Use the `operator` name required by C++ syntax for overloaded operators.
- Use the required `operator Type` syntax for user-defined conversion functions, with `Type` following the type naming rules.
- If adding a user-defined literal operator, use a `_lower_snake_case` suffix.
- Follow the required names for functions specified by the language or an external interface, such as `main`, standard-library replacement functions, and third-party callback entry points.

Examples:

- `Setup`
- `LoadFromFile`
- `Start`
- `Mount`
- `GenerateOperateId`
- `CreateLocalConfigWithCurrentDefaults`
- `operator==`
- `operator bool`
- `operator ""_mb`

### C API Functions

- Use `snake_case` for exported C APIs and C-style utility functions.

Examples:

- `mmc_setup`
- `mmc_init`
- `mmc_uninit`
- `smem_bm_config_init`
- `smem_init`

> **C API struct typedefs**: `struct` typedefs in exported C headers (such as `mmc_tls_config` and `mmc_client_config_t`) are part of the external API. To preserve compatibility, they do not need to be renamed. New C API struct typedefs should use `snake_case`, consistent with function names.

### Parameters

- Use `lowerCamelCase` for parameters.
- Apply this rule to function, template, lambda, and catch parameters.
- Omit unused parameter names when possible; if a name must be retained, use `lowerCamelCase`.

Examples:

- `defaultTtl`
- `config`
- `filePath`
- `urlString`

### Local Variables

- Use `lowerCamelCase` for local variables.
- Prefer `std::string` for strings. Use `char*` / `const char*` only when required by a C API, low-level compatibility, or an external interface.
- Apply this rule to references, pointers, arrays, range-for variables, and structured-binding variables.
- Apply this rule to variables that hold lambda objects.

Examples:

- `tmpStr`
- `localTime`
- `tlsConfig`
- `buffer`
- `statusRef`
- `bufferPtr`
- `objectIds`
- `[rankId, deviceId]`

```cpp
auto processCallback = [config](int value) { /* ... */ };
auto onComplete = [&]() { /* ... */ };
```

### Member Variables

- Use `lowerCamelCase` for `public` member variables.
- Use `lowerCamelCase_` for `private` / `protected` member variables, with a trailing underscore.
- Apply these rules to non-static data members, non-constant static data members, and bit fields.
- Name class static constants according to the non-local constant rules.
- Existing names may be retained for compatibility with a public interface or external ABI.

Examples:

- `rank`
- `mediaType`
- `defaultTtlMs`
- `rank_`
- `mediaType_`
- `started_`
- `defaultTtlMs_`
- `retryCount_`
- `kDefaultTtlMs`

### Global Variables

- Prefix global variables with `g`.
- Use `UpperCamelCase` for the name after the prefix.
- Introduce global state only when necessary.
- Apply the global-variable rules to mutable variables at namespace scope.
- Apply the global, member, or local variable rules to mutable `thread_local` variables according to their scope.

Examples:

- `gOneNumber`
- `gRequestIdGenerator`
- `gDefaultTimeoutMs`
- `gLogger`

### Function-Local Static Variables

- Use `lowerCamelCase` for function-local static variables.
- Do not use the `g_` prefix.
- Name function-local static constants according to the local constant rules.

Examples:

- `localTime`
- `retryCount`
- `cacheReady`

## Constants, Macros, and Enumerations

### Non-local Constants

- Prefix non-local constants with `k` and use `UpperCamelCase` for the rest of the name.
- This includes global constants, namespace constants, class static constants, and constants reused across functions.
- If a variable template represents a constant, prefix it with `k` and use `UpperCamelCase`.

Examples:

- `kConfMust`
- `kDramSizeAlignment`
- `kHbmSizeAlignment`
- `kReturnOk`
- `kHttpTestPortBase`
- `kHttpTestRetryCount`
- `kHttpTestRetryIntervalMs`
- `kLogRotationFileSize`
- `kEvictThresholdHigh`
- `kEvictThresholdLow`

### Local Constants

- Use `lowerCamelCase` for local constants.
- Retain special names only when required for compatibility with an existing interface or protocol.

Examples:

- `defaultTtl`
- `retryCount`

### Macros

- Use `UPPER_SNAKE_CASE` for macro names, function-like macro names, and macro parameters.
- Macro names must have clear meaning.

Examples:

- `MMC_DATA_TTL_MS`
- `MMC_LOG_ERROR`
- `MMC_ASSERT`
- `TP_TRACE_BEGIN`
- `LOG_ERROR`

### Enumeration Types

- Use `UpperCamelCase` for enumeration type names.
- Use anonymous enumerations only for localized compatibility cases. Prefer named enumerations or named constants.

Examples:

- `MmcErrorCode`
- `EvictResult`
- `MemUnit`
- `ConfValueType`

### Enumeration Values

- Use `UPPER_SNAKE_CASE` for enumeration values, including unscoped enumerations declared in a class.

Examples:

- `MMC_OK`
- `MMC_ERROR`
- `MEDIA_HBM`
- `REMOVE`
- `MOVE_DOWN`
- `ACL_MEM_LOCATION_TYPE_HOST`

## Templates and Other C++17 Language Features

### Template Names

- Use `UpperCamelCase` for class templates, alias templates, and function templates.
- Name variable templates according to whether they represent variables or constants.
- Name member templates according to the declared entity: member function, member variable, type, or alias.
- Explicit and partial specializations must use the same naming style as the primary template.

Examples:

- `ObjectPool`
- `MmcUbsIoProxyPtr`
- `CreateLocalConfig`
- `kDefaultLimit`

### Template Parameters

- Use `UpperCamelCase` for type template parameters.
- Short generic type parameters may use `T`, `U`, or `V`.
- Use `lowerCamelCase` for non-type template parameters.
- Use `UpperCamelCase` for template template parameters.
- Name parameter packs according to their parameter category. Prefer a clear plural name or one that uses `...`.

Examples:

- `ValueType`
- `Allocator`
- `bufferSize`
- `Container`
- `ValueTypes`
- `args`

### Lambda Captures

- Lambda captures do not introduce a new naming style; captured variables keep their original names.
- Use `lowerCamelCase` for new variables introduced by init-capture.

Examples:

- `[config]`
- `[localConfig = config]`

### Using Declarations and Directives

- `using` declarations and `using namespace` directives do not rename entities; imported entities keep their original names.
- Do not add `using namespace` to headers.

### Custom Attributes

- Do not add custom attributes or custom attribute namespaces.
- If an external interface requires one for compatibility, follow that interface's naming requirements.

## Resource Management

### Smart Pointers

- Prefer `std::make_unique` to create `std::unique_ptr` instances.
- Prefer `std::make_shared` to create `std::shared_ptr` instances.
- Do not create objects with `new` directly.
- To handle allocation failure, use `new (std::nothrow)` and immediately wrap the result in a smart pointer.

## Test and Mock Naming

### Test Files

- Use `snake_case` for test filenames.
- Name all new test files with the `*_test.cpp` suffix, which follows common C/C++ open-source conventions.
- Existing test files (such as `test_meta_manager.cpp`) do not need to be renamed immediately. New test files must use the `*_test.cpp` suffix.

Examples:

- `mf_file_util_test.cpp`
- `smem_last_error_test.cpp`
- `mmc_configuration_test.cpp`
- `meta_manager_test.cpp`

### Mock Files

- Use the `mock_*.cpp` pattern for Mock implementation files.

Examples:

- `mock_dfc_api.cpp`
- `mock_smem.cpp`
- `mock_smem_bm.cpp`

### Test Fixture Types

- Name test fixture classes using `UpperCamelCase` followed by `Test`.

Examples:

- `MetaConfigUtilsTest`
- `MmcConfigurationTest`
- `BmInitTest`
- `MmcMetaManagerTest`

### Test Case Names

- Use behavior-oriented `UpperCamelCase` names for test cases.
- Names should clearly express the precondition, action, and expected result. Avoid generic names that convey too little information.

Examples:

- `CreateDefaultMetaConfigReturnsExpectedDefaults`
- `ValidateTLSConfigWithMissingCertReturnsError`

## Applying the Conventions

### Static Checks

- Use `clang-format` to format code; the repository root contains the `.clang-format` configuration.
- Use `clang-tidy` to check naming and code quality, adding checks as needed.
- After modifying or adding code, run `git diff HEAD -U0 | clang-format-diff -p1 -i` to format changed lines.
