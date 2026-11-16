/*
 * Copyright (c) Huawei Technologies Co., Ltd. 2025-2025. All rights reserved.
 * MemCache_Hybrid is licensed under Mulan PSL v2.
 * You can use this software according to the terms and conditions of the Mulan PSL v2.
 * You may obtain a copy of Mulan PSL v2 at:
 *          http://license.coscl.org.cn/MulanPSL2
 * THIS SOFTWARE IS PROVIDED ON AN "AS IS" BASIS, WITHOUT WARRANTIES OF ANY KIND,
 * EITHER EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO NON-INFRINGEMENT,
 * MERCHANTABILITY OR FIT FOR A PARTICULAR PURPOSE.
 * See the Mulan PSL v2 for more details.
*/

#ifndef MMC_CONFIG_VALIDATOR_H
#define MMC_CONFIG_VALIDATOR_H

#include <algorithm>
#include <cctype>
#include <set>
#include <string>
#include <vector>
#include <unistd.h>
#include <utility>
#include <sys/stat.h>

#include "mmc_ref.h"
#include "mmc_functions.h"

namespace ock {
namespace mmc {
class Validator : public MmcReferable {
public:
    ~Validator() override = default;

    virtual bool Initialize() = 0;
    virtual bool Validate(const std::string &)
    {
        return true;
    }

    virtual bool Validate(int)
    {
        return true;
    }

    virtual bool Validate(float)
    {
        return true;
    }

    virtual bool Validate(long unsigned)
    {
        return true;
    }

    const std::string &ErrorMessage()
    {
        return mErrMsg;
    }

protected:
    explicit Validator(std::string name) : mName(std::move(name)) {}

    std::string mName;
    std::string mErrMsg;
};
using ValidatorPtr = MmcRef<Validator>;

class VNoCheck : public Validator {
public:
    static ValidatorPtr Create(const std::string &name = "")
    {
        return {new (std::nothrow) VNoCheck(name)};
    }

    explicit VNoCheck(const std::string &name) : Validator(name) {}

    ~VNoCheck() override = default;

    bool Initialize() override
    {
        return true;
    }
};

class VStrEnum : public Validator {
public:
    static ValidatorPtr Create(const std::string &name, const std::string &enumStr, const bool caseSensitive = false)
    {
        return {new (std::nothrow) VStrEnum(name, enumStr, caseSensitive)};
    }

    VStrEnum(const std::string &name, std::string enumStr, const bool caseSensitive = false)
        : Validator(name), mEnumString(std::move(enumStr)), caseSensitive(caseSensitive)
    {
        if (!caseSensitive) {
            std::transform(mEnumString.begin(), mEnumString.end(), mEnumString.begin(), tolower);
        }
    }

    ~VStrEnum() override = default;

    bool Initialize() override
    {
        // enum string should like this
        // rc||tcp||xx
        std::set<std::string> validEnumSet;
        SplitStr(mEnumString, "||", validEnumSet);
        if (validEnumSet.empty()) {
            mErrMsg = "Failed to initialize validator for <" + mName + ">, because enum string is not correct";
            return false;
        }
        return true;
    }

    bool Validate(const std::string &value) override
    {
        // if value has ||
        // for example rc||tcp
        if (value.find("||") != std::string::npos) {
            mErrMsg = "Invalid value for <" + mName + ">, it should be one of " + mEnumString;
            return false;
        }

        std::string valueFormatted = value;
        if (!caseSensitive) {
            std::transform(valueFormatted.begin(), valueFormatted.end(), valueFormatted.begin(), tolower);
        }

        // create ||rc||tcp||xx||
        // find ||rc||
        std::string tmp = "||" + mEnumString + "||";
        if (tmp.find("||" + valueFormatted + "||") == std::string::npos) {
            mErrMsg = "Invalid value for <" + mName + ">, it should be one of " + mEnumString;
            return false;
        }
        return true;
    }

private:
    std::string mEnumString;
    bool caseSensitive;
};

class VStrInSet : public Validator {
public:
    static ValidatorPtr Create(const std::string &name, const std::string &enumStr)
    {
        return {new (std::nothrow) VStrInSet(name, enumStr)};
    }

    VStrInSet(const std::string &name, std::string enumStr) : Validator(name), mEnumString(std::move(enumStr)) {}

    ~VStrInSet() override = default;

    bool Initialize() override
    {
        SplitStr(mEnumString, "||", validEnumSet);
        if (validEnumSet.empty()) {
            mErrMsg = "Failed to initialize validator for <" + mName + ">, because enum string is not correct";
            return false;
        }
        return true;
    }

    bool Validate(const std::string &value) override
    {
        std::vector<std::string> validValues;
        SplitStr(value, "|", validValues);
        for (const auto &item : validValues) {
            if (validEnumSet.find(item) == validEnumSet.end()) {
                mErrMsg = "Invalid value for <" + mName + ">, it should be one of " + mEnumString;
                return false;
            }
        }
        return true;
    }

private:
    std::string mEnumString;
    std::set<std::string> validEnumSet;
};

class VStrLayeredProtocol : public Validator {
public:
    static ValidatorPtr Create(const std::string &name, const std::string &enumStr)
    {
        return {new (std::nothrow) VStrLayeredProtocol(name, enumStr)};
    }

    VStrLayeredProtocol(const std::string &name, std::string enumStr) : Validator(name), mEnumString(std::move(enumStr))
    {
        SplitStr(mEnumString, "||", validProtocols);
    }

    ~VStrLayeredProtocol() override = default;

    bool Initialize() override
    {
        SplitStr(mEnumString, "||", validProtocols);
        if (validProtocols.empty()) {
            mErrMsg = "Failed to initialize validator for <" + mName + ">, because protocol enum string is not correct";
            return false;
        }
        return true;
    }

    bool Validate(const std::string &value) override
    {
        if (value.empty() || value.front() == ':' || value.back() == ':') {
            mErrMsg = "Invalid value for <" + mName + ">, protocol layers cannot be empty";
            return false;
        }

        std::vector<std::string> layers;
        SplitStr(value, ":", layers);

        for (const auto &layer : layers) {
            if (!layer.empty() && validProtocols.find(layer) == validProtocols.end()) {
                mErrMsg = "Invalid value for <" + mName + ">, protocol layer should be one of " + mEnumString;
                return false;
            }
        }
        if (std::all_of(layers.begin(), layers.end(), [](const std::string &layer) { return layer.empty(); })) {
            mErrMsg = "Invalid value for <" + mName + ">, protocol must contain at least one valid layer";
            return false;
        }
        return true;
    }

private:
    std::string mEnumString;
    std::set<std::string> validProtocols;
};

class VStrNotNull : public Validator {
public:
    static ValidatorPtr Create(const std::string &name)
    {
        return {new (std::nothrow) VStrNotNull(name)};
    }

    explicit VStrNotNull(const std::string &name) : Validator(name) {};

    ~VStrNotNull() override = default;

    bool Initialize() override
    {
        return true;
    }

    bool Validate(const std::string &value) override
    {
        if (value.empty()) {
            mErrMsg = "Invalid value for <" + mName + ">, it should not be empty";
            return false;
        }
        return true;
    }
};

class VStrLength : public Validator {
public:
    static ValidatorPtr Create(const std::string &name, const unsigned long lenLimit)
    {
        return {new (std::nothrow) VStrLength(name, lenLimit)};
    }

    explicit VStrLength(const std::string &name, const unsigned long lenLimit)
        : Validator(name), mLengthLimit(lenLimit) {};

    ~VStrLength() override = default;

    bool Initialize() override
    {
        return true;
    }

    bool Validate(const std::string &value) override
    {
        if (value.length() > mLengthLimit) {
            mErrMsg = "String length exceeds limit for <" + mName + ">, it should be no longer than " +
                      std::to_string(mLengthLimit);
            return false;
        }
        return true;
    }

private:
    unsigned long mLengthLimit;
};

class VIntRange : public Validator {
public:
    static ValidatorPtr Create(const std::string &name, const int &start, const int &end)
    {
        return {new (std::nothrow) VIntRange(name, start, end)};
    }
    VIntRange(const std::string &name, const int &start, const int &end) : Validator(name), mStart(start), mEnd(end) {};

    ~VIntRange() override = default;

    bool Initialize() override
    {
        if (mStart >= mEnd) {
            mErrMsg = "Failed to initialize validator for <" + mName + ">, because end should be bigger than start";
            return false;
        }
        return true;
    }

    bool Validate(int value) override
    {
        if (value < mStart || value > mEnd) {
            if (mEnd == INT32_MAX) {
                mErrMsg = "Invalid value for <" + mName + ">, it should be >= " + std::to_string(mStart);
            } else {
                mErrMsg = "Invalid value for <" + mName + ">, it should be between " + std::to_string(mStart) + "-" +
                          std::to_string(mEnd);
            }
            return false;
        }

        return true;
    }

private:
    int mStart;
    int mEnd;
};

class VUInt64Range : public Validator {
public:
    static ValidatorPtr Create(const std::string &name, const uint64_t &start, const uint64_t &end)
    {
        return {new (std::nothrow) VUInt64Range(name, start, end)};
    }
    VUInt64Range(const std::string &name, const uint64_t &start, const uint64_t &end)
        : Validator(name), mStart(start), mEnd(end) {};

    ~VUInt64Range() override = default;

    bool Initialize() override
    {
        if (mStart >= mEnd) {
            mErrMsg = "Failed to initialize validator for <" + mName + ">, because end should be bigger than start";
            return false;
        }
        return true;
    }

    bool Validate(const uint64_t value) override
    {
        if (value < mStart || value > mEnd) {
            mErrMsg = "Invalid value for <" + mName + ">, it should be between " + std::to_string(mStart) + "-" +
                      std::to_string(mEnd);
            return false;
        }

        return true;
    }

private:
    uint64_t mStart;
    uint64_t mEnd;
};

class VPathAccess : public Validator {
public:
    static ValidatorPtr Create(const std::string &name, int flag)
    {
        return {new (std::nothrow) VPathAccess(name, flag)};
    }

    VPathAccess(const std::string &name, int flag) : Validator(name)
    {
        mFlag = flag;
    }

    ~VPathAccess() override = default;

    bool Initialize() override
    {
        return true;
    }

    bool Validate(const std::string &path) override
    {
        if (path.empty()) {
            mErrMsg = "Invalid value for " + mName + ", path is empty.";
            return false;
        }
        std::string fullPath = path;
        if (path.at(path.size() - 1) != '/') {
            fullPath += "/";
        }
        size_t posLeft = 0;
        size_t posRight = 0;
        std::string existDeepestDir;
        while (posLeft < fullPath.size()) {
            posRight = fullPath.find('/', posLeft);
            if (posRight == std::string::npos) {
                break;
            }
            if (posRight > posLeft && posLeft > 0) {
                // 检查更深一层路径是否存在
                std::string tmp = existDeepestDir + "/" + fullPath.substr(posLeft, posRight - posLeft);
                if (access(tmp.c_str(), F_OK) != 0) {
                    break;
                }
                existDeepestDir = std::string(tmp); // 记录当前存在的路径
            }
            posLeft = posRight + 1;
        }
        return PathCheck(existDeepestDir, fullPath.substr(posRight), path);
    }

private:
    bool PathCheck(const std::string &existDeepestDir, const std::string &rest, const std::string &path)
    {
        // 检查路径是否合法
        char realBinPath[PATH_MAX + 1] = {0x00};
        if (existDeepestDir.length() > PATH_MAX) {
            return false;
        }
        char *tmp = realpath(existDeepestDir.c_str(), realBinPath);
        if (tmp == nullptr) {
            mErrMsg = "Invalid value (" + path + ") for <" + mName + ">. Permission denied.";
            return false;
        }
        // 检查路径是否有权限访问
        if (access(existDeepestDir.c_str(), mFlag) != 0) {
            mErrMsg = "Invalid value (" + path + ") for <" + mName + ">. Permission denied.";
            return false;
        }
        // 检查待创建路径是否存在禁止字符
        if (!rest.empty()) {
            for (auto &forbidden : forbiddenWords) {
                auto pos = rest.find(forbidden + "/");
                if (pos != std::string::npos) {
                    mErrMsg = "Invalid value (" + path + ") for <" + mName +
                              ">, "
                              "whose part need to be automatically created should not contain '" +
                              forbidden + "'.";
                    return false;
                }
            }
        }
        return true;
    }

    int mFlag = 0;
    const std::vector<std::string> forbiddenWords{".."};
};

// Generic URL port validation accepts any scheme (tcp/http/https/etcd/...) without parsing userinfo, host,
// or IPv6 brackets. It reads the port digits after the last colon and checks [minPort, maxPort].
// Paths or parameters may follow the port. Empty values are allowed for disabled options;
// each configuration option determines whether a value is required.
class VUrlPort : public Validator {
public:
    static constexpr uint32_t kDefaultMinPort = 1025;
    static constexpr uint32_t kDefaultMaxPort = 65535;

    static ValidatorPtr Create(const std::string &name = "", uint32_t minPort = kDefaultMinPort,
                               uint32_t maxPort = kDefaultMaxPort)
    {
        return {new (std::nothrow) VUrlPort(name, minPort, maxPort)};
    }

    VUrlPort(const std::string &name, uint32_t minPort, uint32_t maxPort)
        : Validator(name), mMinPort(minPort), mMaxPort(maxPort)
    {}

    ~VUrlPort() override = default;

    bool Initialize() override
    {
        return true;
    }

    bool Validate(const std::string &value) override
    {
        constexpr const char *kSchemeSuffix = "://";
        constexpr size_t kSchemeSuffixLen = 3;

        if (value.empty()) {
            return true;
        }

        const size_t schemePos = value.find(kSchemeSuffix);
        if (schemePos == std::string::npos || schemePos == 0) {
            mErrMsg = "Invalid value (" + value + ") for <" + mName + ">, expected format <scheme>://<host>:<port>.";
            return false;
        }

        const std::string hostPort = value.substr(schemePos + kSchemeSuffixLen);
        const size_t colonPos = hostPort.rfind(':');
        if (colonPos == std::string::npos || colonPos + 1 >= hostPort.size()) {
            mErrMsg = "Invalid value (" + value + ") for <" + mName + ">, port is missing.";
            return false;
        }

        uint32_t port = 0;
        size_t digitCount = 0;
        for (size_t i = colonPos + 1; i < hostPort.size(); ++i) {
            const char c = hostPort[i];
            if (c < '0' || c > '9') {
                break;
            }
            port = port * 10U + static_cast<uint32_t>(c - '0');
            ++digitCount;
            if (port > mMaxPort) {
                break;
            }
        }
        if (digitCount == 0) {
            mErrMsg = "Invalid value (" + value + ") for <" + mName + ">, port must be numeric.";
            return false;
        }
        if (port < mMinPort || port > mMaxPort) {
            mErrMsg = "Invalid value (" + value + ") for <" + mName + ">, port must be in [" +
                      std::to_string(mMinPort) + ", " + std::to_string(mMaxPort) + "].";
            return false;
        }
        return true;
    }

private:
    uint32_t mMinPort = kDefaultMinPort;
    uint32_t mMaxPort = kDefaultMaxPort;
};
} // namespace mmc
} // namespace ock

#endif
