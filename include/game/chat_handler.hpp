#pragma once

#include <deque>

#include "game/chat_filters.hpp"

#include "game/world_packets.hpp"
#include "game/opcode_table.hpp"
#include "game/handler_types.hpp"
#include "network/packet.hpp"
#include <deque>
#include <fstream>
#include <functional>
#include <optional>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <map>
#include <set>
#include <vector>

namespace wowee {
namespace game {

class GameHandler;
struct TextSubject;

class ChatHandler {
public:
    using PacketHandler = std::function<void(network::Packet&)>;
    using DispatchTable = std::unordered_map<LogicalOpcode, PacketHandler>;

    explicit ChatHandler(GameHandler& owner);

    void registerOpcodes(DispatchTable& table);

    // --- Public API (delegated from GameHandler) ---
    void sendChatMessage(ChatType type, const std::string& message, const std::string& target = "");
    void sendAddonMessage(ChatType type, const std::string& message, const std::string& target = "");
    void sendTextEmote(uint32_t textEmoteId, uint64_t targetGuid = 0);
    void joinChannel(const std::string& channelName, const std::string& password = "", uint32_t channelId = 0);
    void leaveChannel(const std::string& channelName);
    /// Ask the server for a channel's members; the answer arrives as
    /// SMSG_CHANNEL_LIST and fires CHANNEL_ROSTER_UPDATE.
    /// `print`: /chatlist asked, so the answer is printed as CHAT_MSG_CHANNEL_LIST.
    void requestChannelList(const std::string& channelName, bool print = false);
    [[nodiscard]] std::string getChannelByIndex(int index) const;
    [[nodiscard]] int getChannelIndex(const std::string& channelName) const;
    /// Joined channels by chat number: slot i is number i + 1. A slot left
    /// empty ("") is a number given up by leaving, kept free until a join
    /// takes it: 2.4.3 keeps "5. LookingForGroup" after "4. GuildRecruitment"
    /// is left and gives 4 to the next channel joined (pair 15).
    [[nodiscard]] const std::vector<std::string>& getJoinedChannels() const { return joinedChannels_; }
    /// ChatChannels.dbc id of a joined channel, 0 for a custom one.
    [[nodiscard]] uint32_t joinedChannelId(const std::string& channelName) const {
        const auto it = joinedChannelIds_.find(channelName);
        return it == joinedChannelIds_.end() ? 0 : it->second;
    }
    /// The ChatChannels.dbc row whose name is `shortName` ("General",
    /// "LookingForGroup"), 0 when none.
    [[nodiscard]] uint32_t builtinChannelId(const std::string& shortName) const;
    /// ChatChannels.dbc short name by id ("General"), empty for none.
    [[nodiscard]] std::string builtinChannelName(uint32_t id) const;
    /// ChatChannels.dbc flags by id, 0 for none.
    [[nodiscard]] uint32_t builtinChannelFlags(uint32_t id) const;
    /// Join, or leave, every built-in channel carrying ChatChannels flag
    /// `flag` (kChannelNoGuild: GuildRecruitment) under the name the zone
    /// gives it now.
    void joinBuiltinChannels(uint32_t flag);
    void leaveBuiltinChannels(uint32_t flag);
    /// Apply a live change to one of the Chat > Channels to join switches
    /// ("joingeneral", "jointrade", "joinlocaldefense", "joinlfg", "joinlocal").
    void applyAutoJoinSetting(const std::string& key, bool enabled);
    /// ChatChannels.dbc flags (2.4.3 Flags column).
    static constexpr uint32_t kChannelInitial  = 0x00001;  ///< joined at login
    static constexpr uint32_t kChannelZoneDep  = 0x00002;  ///< named after the zone
    static constexpr uint32_t kChannelCityOnly = 0x00020;  ///< "- City" in a capital
    static constexpr uint32_t kChannelNoGuild  = 0x20000;  ///< not for guild members
    /// Whether the player owns this channel - the one who created it, or who
    /// it passed to. SMSG_CHANNEL_NOTIFY says so with OWNER_CHANGED, which
    /// carries the new owner's guid: AzerothCore's Channel::MakeOwnerChanged
    /// writes _ownerGUID into it.
    [[nodiscard]] bool ownsChannel(const std::string& name) const {
        return ownedChannels_.count(name) != 0;
    }
    /// Who sent the line with this id, or zero if it is not remembered.
    [[nodiscard]] uint64_t chatLineSender(uint32_t lineId) const {
        for (const auto& [id, guid] : chatLineSenders_)
            if (id == lineId) return guid;
        return 0;
    }

    /// The roster of a channel by name, empty until a list is asked for. The
    /// list used to be printed to chat and dropped, so the channel panel had
    /// no members to show and reported every channel as having none.
    [[nodiscard]] const std::vector<ChannelMember>& getChannelRoster(const std::string& channel) const {
        static const std::vector<ChannelMember> empty;
        auto it = channelRosters_.find(channel);
        return (it != channelRosters_.end()) ? it->second : empty;
    }
    /// One row of the channel panel as 2.4.3 lays it out (pair 11): the
    /// Group, World and Custom headers, each followed by its channels unless
    /// collapsed. World holds the built-in channels (a ChatChannels.dbc id),
    /// Custom the rest; this client joins no group (voice) channels.
    struct ChannelDisplayRow {
        std::string label;        ///< header word, or the channel's short name
        bool header = false;
        bool collapsed = false;   ///< headers only
        int children = 0;         ///< headers only: channels under it
        std::string channel;      ///< channel rows only: the joined name
        int number = 0;           ///< channel rows only: its chat number
        const char* category = nullptr;  ///< channel rows only
    };
    [[nodiscard]] std::vector<ChannelDisplayRow> channelDisplayRows() const;
    /// The row a joined channel is on, 0 when it is not shown (or collapsed).
    [[nodiscard]] int channelDisplayIndex(const std::string& channelName) const;
    /// Expand or collapse the header on a display row; other rows are ignored.
    void setChannelHeaderCollapsed(int displayIndex, bool collapsed);
    /// The joined channel a name addresses: the full name, or the short one
    /// the panel shows for a built-in channel ("General" for "General -
    /// Stormwind City"). Empty when none.
    [[nodiscard]] std::string resolveChannelName(const std::string& name) const;
    /// Members the realm last counted (SMSG_CHANNEL_MEMBER_COUNT or a
    /// roster), or nullopt before either arrived.
    [[nodiscard]] std::optional<uint32_t> channelMemberCount(const std::string& channelName) const;
    /// CMSG_GET_CHANNEL_MEMBER_COUNT for every joined channel.
    void requestChannelMemberCounts();
    /// CMSG_CHANNEL_DISPLAY_LIST: the roster the panel shows on a click.
    void requestChannelDisplayList(const std::string& channelName);
    /// The roster the panel shows for a row's channel: the realm's list for
    /// a joined channel, the group itself for the Group row (named by
    /// kGroupChannel), sorted by name with the leader as owner and moderator
    /// (pair 11: "Edrick", then "Edrikk" 1, 1 in a raid Edrikk led).
    [[nodiscard]] std::vector<ChannelMember> channelPanelRoster(const std::string& channel) const;
    /// What a Group row's `channel` holds: no realm channel can be named so.
    static constexpr const char* kGroupChannel = "\x01group";
    /// A name query answered: fill in channel roster rows waiting on it.
    void onPlayerName(uint64_t guid, const std::string& name);
    void autoJoinDefaultChannels();
    /// The zone is known: join the built-in channels requested at world entry.
    void joinZoneChannels(uint32_t zoneId);
    /// The interface has loaded (PLAYER_ENTERING_WORLD fired).
    void onInterfaceReady(uint32_t liveZoneId);
    void addLocalChatMessage(const MessageChatData& msg);

    /// Put a line of a given kind into chat, locally.
    ///
    /// The kind matters: CHAT_MSG_MONEY, CHAT_MSG_COMBAT_XP_GAIN and their like
    /// are chat messages, and the interface's handler reads twelve arguments
    /// off every one of them - arg4 is a channel name and it calls strlen on it
    /// without checking. Firing one of these with just the text raised there and
    /// took the rest of the handler with it, so the line never appeared at all.
    void addLocalChatLine(ChatType type, const std::string& message);
    void addSystemChatMessage(const std::string& message);

    /// Announces a message as CHAT_MSG_<TYPE> with WoW's argument order. Both
    /// the server path and the local one go through here, because an interface
    /// cannot tell the difference and should not have to.
    void fireChatEvent(const MessageChatData& msg);
    void toggleAfk(const std::string& message);
    void toggleDnd(const std::string& message);
    /// PLAYER_FLAGS AFK (0x02) and DND (0x04) of the local player; prints the
    /// original's away lines when `announce` (an update, not the create block).
    void onAwayFlags(bool afk, bool dnd, bool announce);
    /// CHAT_MSG_CHANNEL_NOTICE(_USER) for one SMSG_CHANNEL_NOTIFY.
    void fireChannelNotice(const ChannelNotifyData& data, int number, bool waited = false, bool changed = false);
    /// Fire the held notices about `guid`, or every expired one.
    void flushNoticesAwaitingName(uint64_t guid, bool expired);
    void replyToLastWhisper(const std::string& message);

    // ---- Methods moved from GameHandler ----
    void submitGmTicket(const std::string& text);
    /// Replace the text of the ticket already open.
    void updateGmTicket(const std::string& text);

    /// Release the lines held for `guid`, now that its name is known.
    void flushChatAwaitingName(uint64_t guid);
    /// Release anything whose wait has run out, named or not. A line nobody
    /// answers for is still a line the player should see.
    void expireChatAwaitingName();
    void handleMotd(network::Packet& packet);

    // --- State accessors ---
    std::deque<MessageChatData>& getChatHistory() { return chatHistory_; }
    [[nodiscard]] const std::deque<MessageChatData>& getChatHistory() const { return chatHistory_; }
    [[nodiscard]] size_t getMaxChatHistory() const { return maxChatHistory_; }
    void setMaxChatHistory(size_t n) { maxChatHistory_ = n; }

    // Chat auto-join settings (aliased from handler_types.hpp)
    using ChatAutoJoin = game::ChatAutoJoin;
    ChatAutoJoin chatAutoJoin;

private:
    void emitChatBubble(const MessageChatData& data);

    /// A chat line waiting for a name: its sender's, or that of the player an
    /// NPC's line is written for. See deliverChatMessage.
    struct ChatAwaitingName {
        MessageChatData data;
        uint64_t guid = 0;
        double deadline = 0.0;
    };
    std::deque<ChatAwaitingName> chatAwaitingName_;
    /// A channel notice naming a player not known yet, held for the name
    /// query as a chat line is (fireChannelNotice).
    struct ChannelNoticeAwaitingName {
        ChannelNotifyData data;
        int number = 0;
        uint64_t guid = 0;
        double deadline = 0.0;
    };
    std::deque<ChannelNoticeAwaitingName> noticesAwaitingName_;
    /// Long enough for a name query to come back on a working connection,
    /// short enough that a line does not feel withheld.
    static constexpr double kNameWaitSeconds = 1.0;
    // --- Packet handlers ---
    void handleMessageChat(network::Packet& packet);
    /// Deliver a parsed chat line: resolve its sender, filter it, store it and
    /// tell the interface. Split from handleMessageChat so a line held back
    /// for its sender's name can be run through it again once the name lands.
    void deliverChatMessage(MessageChatData data, bool alreadyWaited);
    /// A player's name from whatever already holds it - the name cache, the
    /// player nearby, the party, the guild roster. Empty when none does.
    [[nodiscard]] std::string knownPlayerName(uint64_t guid) const;
    /// Subject metadata ($n/$N/$c/$C/$r/$R/$g) for a monster or raid-boss line
    /// addressed to a non-zero receiverGuid.
    [[nodiscard]] TextSubject monsterLineSubject(const MessageChatData& data) const;

    void handleTextEmote(network::Packet& packet);
    void handleChannelNotify(network::Packet& packet);
    void handleChannelList(network::Packet& packet);
    void handleUserlistAdd(network::Packet& packet);
    void handleUserlistRemove(network::Packet& packet);
    void applyUserlistChange(const std::string& chanName, uint64_t guid,
                             uint8_t memberFlags, bool removing);
    void initializeChatLog();
    void logChatMessage(const MessageChatData& msg, const char* source);

    GameHandler& owner_;

    // --- State ---
    std::deque<MessageChatData> chatHistory_;
    /// What has just been said, for the spam filter. Short on purpose: this
    /// is looking for a line pasted again a moment later, not keeping a record
    /// of the conversation. See repeatsRecentLine in chat_filters.hpp.
    std::deque<RecentChatLine> recentChatLines_;
    size_t maxChatHistory_ = 100;
    uint64_t chatUidCounter_ = 0;  // monotonic uid for MessageChatData::uid
    std::vector<std::string> joinedChannels_;
    /// ChatChannels.dbc id of each joined channel by name (YOU_JOINED), 0 for
    /// a custom one: a zone change replaces a channel with the same id in place.
    std::map<std::string, uint32_t> joinedChannelIds_;
    /// World entry asked for the built-in channels; they are joined once the
    /// zone is known (SMSG_INIT_WORLD_STATES), named after it.
    bool zoneChannelsPending_ = false;
    /// Whether the interface is up to print their "Joined Channel" lines;
    /// the zone is GameHandler's (SMSG_INIT_WORLD_STATES).
    bool zoneChannelsInterfaceReady_ = false;
    uint32_t zoneChannelsLiveZone_ = 0;
    void tryJoinZoneChannels();
    /// Put a newly joined channel on the lowest free chat number.
    void takeChannelNumber(const std::string& channelName);
    std::set<std::string> ownedChannels_;
    /// Collapsed panel headers by label ("World", "Custom").
    std::set<std::string> collapsedChannelHeaders_;
    std::map<std::string, uint32_t> channelMemberCounts_;
    /// One ChatChannels.dbc row: id, flags, name pattern ("General - %s")
    /// and short name ("General").
    struct BuiltinChannel {
        uint32_t id = 0;
        uint32_t flags = 0;
        std::string pattern;
        std::string name;
    };
    /// Every ChatChannels.dbc row, loaded on first use.
    mutable std::vector<BuiltinChannel> builtinChannels_;
    const std::vector<BuiltinChannel>& builtinChannels() const;
    /// The name a built-in channel is joined under in `zoneId`: the pattern
    /// with the zone's AreaTable name, or "City" for a city-only channel in a
    /// capital. Empty when the zone has no name.
    std::string builtinChannelJoinName(const BuiltinChannel& channel, uint32_t zoneId) const;
    /// The zone the built-in channels are named after (see tryJoinZoneChannels).
    uint32_t channelZoneId() const;
    void handleChannelMemberCount(network::Packet& packet);
    /// Channels whose next SMSG_CHANNEL_LIST /chatlist asked to print.
    std::set<std::string> printChannelLists_;
    /// Who sent the message a chat line id names, newest last.
    ///
    /// Every chat event carries a line id as its eleventh argument, and
    /// FrameXML builds it into the player link on the name - so a right-click
    /// on that name hands the id back and expects the client to know which
    /// message it was. This client sent zero for all of them, which is one id
    /// shared by every line ever printed and therefore no id at all.
    ///
    /// Bounded, because a session's chat is unbounded and only recent lines can
    /// be reported anyway.
    std::deque<std::pair<uint32_t, uint64_t>> chatLineSenders_;
    uint32_t nextChatLineId_ = 1;
    std::unordered_map<std::string, std::vector<ChannelMember>> channelRosters_;
    bool chatLogEnabled_ = false;
    bool chatLogInitialized_ = false;
    std::string chatLogPath_;
    std::ofstream chatLogStream_;

};

} // namespace game
} // namespace wowee
