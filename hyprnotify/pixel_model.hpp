#pragma once

#include <algorithm>
#include <array>
#include <cctype>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_set>
#include <vector>

namespace NHyprnotify::Pixel {

    enum class eExpansion : uint8_t {
        COLLAPSED,
        SYSTEM_EXPANDED,
        USER_EXPANDED,
    };

    inline constexpr size_t   MAX_CONVERSATION_MESSAGES           = 32;
    inline constexpr size_t   MAX_PRESENTED_CONVERSATION_MESSAGES = 7;
    inline constexpr size_t   MAX_CONVERSATION_PARTICIPANTS       = 16;
    inline constexpr uint32_t MAX_UNREAD_COUNT                    = 999;

    enum class eMessageMutation : uint8_t {
        NONE,
        INSERTED,
        REPLACED,
    };

    struct SAvatarColor {
        double r = 0;
        double g = 0;
        double b = 0;

        bool   operator==(const SAvatarColor&) const = default;
    };

    inline size_t maxVisibleChildren(bool bundle, eExpansion state) {
        if (bundle) {
            switch (state) {
                case eExpansion::COLLAPSED: return 0;
                case eExpansion::SYSTEM_EXPANDED: return 30;
                case eExpansion::USER_EXPANDED: return 50;
            }
        }
        switch (state) {
            case eExpansion::COLLAPSED: return 2;
            case eExpansion::SYSTEM_EXPANDED: return 5;
            case eExpansion::USER_EXPANDED: return 8;
        }
        return 2;
    }

    inline std::string normalizeSection(std::string_view section) {
        if (section == "promotions" || section == "social" || section == "news" || section == "recommendations" || section == "alerting" || section == "silent")
            return std::string{section};
        return {};
    }

    inline std::string normalizeConversationKind(std::string_view kind) {
        if (kind == "one-to-one" || kind == "group")
            return std::string{kind};
        return {};
    }

    inline std::string automaticGroupKey(std::string_view appKey, std::string_view section) {
        const auto append = [](std::string& out, std::string_view value) {
            out.append(std::to_string(value.size()));
            out.push_back(':');
            out.append(value);
        };
        std::string out = "auto:";
        append(out, appKey);
        append(out, section.empty() ? std::string_view{"alerting"} : section);
        return out;
    }

    inline std::string declaredGroupKey(std::string_view appKey, std::string_view groupKey) {
        const auto append = [](std::string& out, std::string_view value) {
            out.append(std::to_string(value.size()));
            out.push_back(':');
            out.append(value);
        };
        std::string out = "declared:";
        append(out, appKey);
        append(out, groupKey);
        return out;
    }

    inline std::string displayGroupKey(std::string_view appKey, std::string_view declaredKey, std::string_view section) {
        return declaredKey.empty() ? automaticGroupKey(appKey, section) : declaredGroupKey(appKey, declaredKey);
    }

    inline std::string conversationKey(std::string_view appKey, std::string_view conversationId) {
        const auto append = [](std::string& out, std::string_view value) {
            out.append(std::to_string(value.size()));
            out.push_back(':');
            out.append(value);
        };
        std::string out = "conversation:";
        append(out, appKey);
        append(out, conversationId);
        return out;
    }

    inline bool classifiedSection(std::string_view section) {
        return section == "promotions" || section == "social" || section == "news" || section == "recommendations";
    }

    inline bool matchesConversation(std::string_view appKey, std::string_view conversationId, std::string_view candidateAppKey, std::string_view candidateConversationId) {
        return !conversationId.empty() && appKey == candidateAppKey && conversationId == candidateConversationId;
    }

    template <typename T>
    inline eMessageMutation upsertMessage(std::vector<T>& messages, std::string_view messageId, std::string_view text,
                                          const std::optional<std::string_view> senderId = std::nullopt, const std::optional<std::string_view> senderName = std::nullopt,
                                          const std::optional<std::string_view> senderIcon = std::nullopt, const std::optional<int64_t> timestampMs = std::nullopt,
                                          const std::optional<bool> historic = std::nullopt) {
        if (text.empty() && messageId.empty())
            return eMessageMutation::NONE;

        const auto IT      = messageId.empty() ? messages.end() : std::ranges::find_if(messages, [&](const auto& message) { return message.id == messageId; });
        T          message = IT == messages.end() ? T{} : *IT;
        message.id         = messageId;
        message.text       = text;
        if (senderId)
            message.senderId = *senderId;
        if (senderName)
            message.senderName = *senderName;
        if (senderIcon)
            message.senderIcon = *senderIcon;
        if (timestampMs)
            message.timestampMs = *timestampMs;
        if (historic)
            message.historic = *historic;

        const auto MUTATION = IT == messages.end() ? eMessageMutation::INSERTED : eMessageMutation::REPLACED;
        if (IT == messages.end())
            messages.push_back(std::move(message));
        else
            *IT = std::move(message);

        std::stable_sort(messages.begin(), messages.end(), [](const auto& a, const auto& b) {
            const auto AT = a.timestampMs > 0 ? a.timestampMs : std::numeric_limits<int64_t>::max();
            const auto BT = b.timestampMs > 0 ? b.timestampMs : std::numeric_limits<int64_t>::max();
            return AT < BT;
        });

        while (messages.size() > MAX_CONVERSATION_MESSAGES) {
            const auto HISTORIC = std::ranges::find_if(messages, [](const auto& entry) { return entry.historic; });
            messages.erase(HISTORIC != messages.end() ? HISTORIC : messages.begin());
        }
        return MUTATION;
    }

    template <typename T>
    inline size_t presentedMessageStart(const std::vector<T>& messages, size_t limit = MAX_PRESENTED_CONVERSATION_MESSAGES) {
        if (limit == 0)
            return messages.size();
        size_t visible = 0;
        for (size_t i = messages.size(); i-- > 0;)
            if (!messages[i].text.empty() && ++visible == limit)
                return i;
        return 0;
    }

    inline uint32_t updatedUnreadCount(uint32_t current, const std::optional<uint32_t> explicitCount, bool historic, eMessageMutation mutation) {
        if (explicitCount)
            return std::min(*explicitCount, MAX_UNREAD_COUNT);
        if (!historic && mutation == eMessageMutation::INSERTED)
            return std::min(current + 1, MAX_UNREAD_COUNT);
        return current;
    }

    inline std::string participantKey(std::string_view senderId, std::string_view senderName) {
        const auto append = [](std::string& out, std::string_view value) {
            out.append(std::to_string(value.size()));
            out.push_back(':');
            out.append(value);
        };
        std::string out = senderId.empty() ? "name:" : "id:";
        append(out, senderId.empty() ? senderName : senderId);
        return out;
    }

    inline std::string firstCodepoint(std::string_view text, size_t at) {
        if (at >= text.size())
            return {};
        const auto C = static_cast<unsigned char>(text[at]);
        size_t     n = C < 0x80 ? 1 : (C & 0xE0) == 0xC0 ? 2 : (C & 0xF0) == 0xE0 ? 3 : (C & 0xF8) == 0xF0 ? 4 : 1;
        n            = std::min(n, text.size() - at);
        return std::string{text.substr(at, n)};
    }

    inline std::string initials(std::string_view name) {
        std::vector<std::string> words;
        bool                     boundary = true;
        for (size_t i = 0; i < name.size();) {
            const auto C = static_cast<unsigned char>(name[i]);
            if (C >= 0x80) {
                if (boundary)
                    words.push_back(firstCodepoint(name, i));
                const auto CP = firstCodepoint(name, i);
                i += std::max<size_t>(1, CP.size());
                boundary = false;
                continue;
            }
            if (std::isalnum(C)) {
                if (boundary) {
                    const char U = static_cast<char>(std::islower(C) ? std::toupper(C) : C);
                    words.emplace_back(1, U);
                }
                boundary = false;
            } else
                boundary = true;
            i++;
        }
        if (words.empty())
            return "?";
        return words.size() == 1 ? words.front() : words.front() + words.back();
    }

    inline uint32_t avatarHash(std::string_view identity) {
        uint32_t H = 2166136261u;
        for (const unsigned char C : identity)
            H = (H ^ C) * 16777619u;
        return H;
    }

    inline SAvatarColor avatarColor(std::string_view identity, bool darkTheme) {
        const double          H = static_cast<double>(avatarHash(identity) % 360u) / 60.0;
        const double          S = darkTheme ? 0.46 : 0.52;
        const double          L = darkTheme ? 0.38 : 0.70;
        const double          C = (1.0 - std::abs(2.0 * L - 1.0)) * S;
        const double          X = C * (1.0 - std::abs(std::fmod(H, 2.0) - 1.0));
        const double          M = L - C / 2.0;
        std::array<double, 3> rgb{};
        if (H < 1)
            rgb = {C, X, 0};
        else if (H < 2)
            rgb = {X, C, 0};
        else if (H < 3)
            rgb = {0, C, X};
        else if (H < 4)
            rgb = {0, X, C};
        else if (H < 5)
            rgb = {X, 0, C};
        else
            rgb = {C, 0, X};
        return {rgb[0] + M, rgb[1] + M, rgb[2] + M};
    }

    inline bool lightAvatarForeground(const SAvatarColor& color) {
        return 0.2126 * color.r + 0.7152 * color.g + 0.0722 * color.b < 0.50;
    }

    template <typename T>
    inline std::vector<size_t> latestDistinctParticipantIndices(const std::vector<T>& messages, size_t limit = MAX_CONVERSATION_PARTICIPANTS) {
        std::vector<size_t>             out;
        std::unordered_set<std::string> seen;
        out.reserve(std::min(limit, messages.size()));
        for (size_t i = messages.size(); i-- > 0 && out.size() < limit;) {
            const auto KEY = participantKey(messages[i].senderId, messages[i].senderName);
            if (KEY == "name:0:" || !seen.insert(KEY).second)
                continue;
            out.push_back(i);
        }
        return out;
    }

} // namespace NHyprnotify::Pixel
