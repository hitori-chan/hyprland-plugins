#include "hyprnotify/pixel_model.hpp"
#include "test.hpp"

#include <string>
#include <vector>

struct SMessageStub {
    std::string id;
    std::string senderId;
    std::string senderName;
    std::string senderIcon;
    std::string text;
    int64_t     timestampMs = 0;
    bool        historic    = false;
};

int main() {
    using namespace NHyprnotify::Pixel;
    NHyprTest::CSuite suite{"pixel_model_test"};

    HYPR_EXPECT(suite, maxVisibleChildren(false, eExpansion::COLLAPSED) == 2);
    HYPR_EXPECT(suite, maxVisibleChildren(false, eExpansion::SYSTEM_EXPANDED) == 5);
    HYPR_EXPECT(suite, maxVisibleChildren(false, eExpansion::USER_EXPANDED) == 8);
    HYPR_EXPECT(suite, maxVisibleChildren(true, eExpansion::COLLAPSED) == 0);
    HYPR_EXPECT(suite, maxVisibleChildren(true, eExpansion::SYSTEM_EXPANDED) == 30);
    HYPR_EXPECT(suite, maxVisibleChildren(true, eExpansion::USER_EXPANDED) == 50);

    HYPR_EXPECT(suite, automaticGroupKey("ab", "c") != automaticGroupKey("a", "bc"));
    HYPR_EXPECT(suite, declaredGroupKey("ab", "c") != declaredGroupKey("a", "bc"));
    HYPR_EXPECT(suite, displayGroupKey("app", "declared", "silent") == declaredGroupKey("app", "declared"));
    HYPR_EXPECT(suite, automaticGroupKey("app", "alerting") != automaticGroupKey("app", "silent"));
    HYPR_EXPECT(suite, conversationKey("ab", "c") != conversationKey("a", "bc"));
    HYPR_EXPECT(suite, conversationKey("app-a", "chat") != conversationKey("app-b", "chat"));
    HYPR_EXPECT(suite, normalizeSection("social") == "social");
    HYPR_EXPECT(suite, normalizeSection("not-a-section").empty());
    HYPR_EXPECT(suite, normalizeConversationKind("group") == "group");
    HYPR_EXPECT(suite, normalizeConversationKind("broadcast").empty());
    HYPR_EXPECT(suite, matchesConversation("org.telegram.desktop", "chat:42", "org.telegram.desktop", "chat:42"));
    HYPR_EXPECT(suite, !matchesConversation("org.telegram.desktop", "chat:42", "org.telegram.desktop", "chat:43"));
    HYPR_EXPECT(suite, !matchesConversation("org.telegram.desktop", "", "org.telegram.desktop", ""));

    HYPR_EXPECT(suite, initials("Ada Lovelace") == "AL");
    HYPR_EXPECT(suite, initials("telegram") == "T");
    HYPR_EXPECT(suite, initials("  42 alerts ") == "4A");
    HYPR_EXPECT(suite, initials("") == "?");
    HYPR_EXPECT(suite, initials("\xE7\x8C\xAB") == "\xE7\x8C\xAB");

    HYPR_EXPECT(suite, participantKey("a", "Same") != participantKey("b", "Same"));
    std::vector<SMessageStub> messages(4);
    messages[0].senderId    = "alice";
    messages[0].senderName  = "Alice";
    messages[1].senderId    = "bob";
    messages[1].senderName  = "Bob";
    messages[2].senderId    = "alice";
    messages[2].senderName  = "Alice";
    messages[3].senderId    = "carol";
    messages[3].senderName  = "Carol";
    const auto participants = latestDistinctParticipantIndices(messages, 2);
    HYPR_EXPECT(suite, (participants == std::vector<size_t>{3, 2}));

    std::vector<SMessageStub> history;
    HYPR_EXPECT(suite,
                upsertMessage(history, "m1", "hello", std::string_view{"alice"}, std::string_view{"Alice"}, std::string_view{"alice.png"}, 10, true) == eMessageMutation::INSERTED);
    HYPR_EXPECT(suite, upsertMessage(history, "m1", "edited") == eMessageMutation::REPLACED);
    HYPR_EXPECT(suite, history.size() == 1);
    HYPR_EXPECT(suite, history[0].text == "edited");
    HYPR_EXPECT(suite, history[0].senderId == "alice" && history[0].senderName == "Alice" && history[0].senderIcon == "alice.png");
    HYPR_EXPECT(suite, history[0].timestampMs == 10 && history[0].historic);

    history.clear();
    for (size_t i = 0; i < MAX_CONVERSATION_MESSAGES; i++)
        HYPR_EXPECT(suite,
                    upsertMessage(history, "m" + std::to_string(i), "body", std::nullopt, std::nullopt, std::nullopt, (int64_t)i + 1,
                                  i == 9 ? std::optional<bool>{true} : std::optional<bool>{false}) == eMessageMutation::INSERTED);
    HYPR_EXPECT(suite, upsertMessage(history, "m32", "body", std::nullopt, std::nullopt, std::nullopt, 33, false) == eMessageMutation::INSERTED);
    HYPR_EXPECT(suite, history.size() == MAX_CONVERSATION_MESSAGES);
    HYPR_EXPECT(suite, std::ranges::none_of(history, [](const auto& message) { return message.id == "m9"; }));
    HYPR_EXPECT(suite, std::ranges::any_of(history, [](const auto& message) { return message.id == "m0"; }));
    HYPR_EXPECT(suite, upsertMessage(history, "m33", "body", std::nullopt, std::nullopt, std::nullopt, 34, false) == eMessageMutation::INSERTED);
    HYPR_EXPECT(suite, std::ranges::none_of(history, [](const auto& message) { return message.id == "m0"; }));

    history.clear();
    HYPR_EXPECT(suite, upsertMessage(history, "late", "late", std::nullopt, std::nullopt, std::nullopt, 30, false) == eMessageMutation::INSERTED);
    HYPR_EXPECT(suite, upsertMessage(history, "early", "early", std::nullopt, std::nullopt, std::nullopt, 10, false) == eMessageMutation::INSERTED);
    HYPR_EXPECT(suite, upsertMessage(history, "middle", "middle", std::nullopt, std::nullopt, std::nullopt, 20, false) == eMessageMutation::INSERTED);
    HYPR_EXPECT(suite, history[0].id == "early" && history[1].id == "middle" && history[2].id == "late");

    std::vector<SMessageStub> presentation(10);
    for (size_t i = 0; i < presentation.size(); i++)
        presentation[i].text = "message " + std::to_string(i);
    HYPR_EXPECT(suite, presentedMessageStart(presentation) == 3);
    presentation[8].text.clear();
    HYPR_EXPECT(suite, presentedMessageStart(presentation) == 2);
    HYPR_EXPECT(suite, presentedMessageStart(presentation, 0) == presentation.size());

    HYPR_EXPECT(suite, updatedUnreadCount(4, std::nullopt, false, eMessageMutation::INSERTED) == 5);
    HYPR_EXPECT(suite, updatedUnreadCount(4, std::nullopt, false, eMessageMutation::REPLACED) == 4);
    HYPR_EXPECT(suite, updatedUnreadCount(4, std::nullopt, true, eMessageMutation::INSERTED) == 4);
    HYPR_EXPECT(suite, updatedUnreadCount(4, MAX_UNREAD_COUNT + 50, false, eMessageMutation::INSERTED) == MAX_UNREAD_COUNT);
    HYPR_EXPECT(suite, updatedUnreadCount(MAX_UNREAD_COUNT, std::nullopt, false, eMessageMutation::INSERTED) == MAX_UNREAD_COUNT);

    HYPR_EXPECT(suite, avatarHash("alice") == avatarHash("alice"));
    HYPR_EXPECT(suite, avatarHash("alice") != avatarHash("bob"));
    HYPR_EXPECT(suite, avatarColor("alice", true) == avatarColor("alice", true));
    const auto dark  = avatarColor("alice", true);
    const auto light = avatarColor("alice", false);
    HYPR_EXPECT(suite, dark != light);
    HYPR_EXPECT(suite, dark.r >= 0 && dark.r <= 1 && dark.g >= 0 && dark.g <= 1 && dark.b >= 0 && dark.b <= 1);
    HYPR_EXPECT(suite, lightAvatarForeground({0.1, 0.1, 0.1}));
    HYPR_EXPECT(suite, !lightAvatarForeground({0.9, 0.9, 0.9}));

    return suite.finish();
}
