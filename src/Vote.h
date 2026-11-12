// Server votes (docs/admin-channel-draft.md "Server votes"). Engine-free so the unit tests can drive
// it: the admin channel tells it which players may vote, which rows reach each player's
// server-message box and which ballots arrive; it answers which players need the vote line
// (again) and when the vote ends.

#ifndef SERVEREXT_VOTE_H
#define SERVEREXT_VOTE_H

#include <cstdio>
#include <map>
#include <string>
#include <vector>

namespace serverext {
namespace vote {

// The retail client's server-message rows, the box the kill feed shares (HudChat row group 1). The
// HUD sets 5 rows and 20 s at init (OpenBF2 Menu/BF2HudManagerInit.cpp:123,132). Every new row
// restarts the group's one timer; when it runs out, the whole group clears within a few frames
// (HudChat::advanceTimers), and a sixth row pushes the oldest out (HudChat::nextRow).
const int kBoxRows = 5;
const double kBoxSeconds = 20.0;
// Re-posting after a quiet clear waits this long past the expected clear, plus the player's ping,
// so the new line never lands before the old one is gone (that would restart the timer and show both).
const double kClearMargin = 0.5;

// The radio messages a retail client sends for PgUp and PgDn when no HUD question is pending
// (OpenBF2 Menu/HudQuestionManager.cpp replyWithoutQuestion: "roger_that" and "negative"). Ids are
// the messages' order in the bf2 content's Common/Sound/VoiceMessages*.con.
const int kYesRadioId = 112;
const int kNoRadioId = 113;

const size_t kMaxText = 96;          // bytes of question text
const int kMinSeconds = 5;
const int kMaxSeconds = 600;

enum Choice { ChoiceNone = 0, ChoiceYes = 1, ChoiceNo = 2 };
enum Outcome { OutcomeNone = 0, OutcomePassed = 1, OutcomeFailed = 2, OutcomeCancelled = 3, OutcomeUnloaded = 4 };

struct Params
{
    std::string text;   // engine text (Windows-1252)
    int seconds;
    int team;           // 0 everyone, else 1 or 2
    int percent;        // yes share of the ballots cast needed to pass, 1..100
    int minVotes;       // ballots needed for a pass
    Params() : seconds(30), team(0), percent(50), minVotes(1) {}
};

class Box
{
public:
    Box() : m_running(false), m_start(0.0), m_end(0.0) {}

    bool running() const { return m_running; }
    const Params& params() const { return m_params; }

    void start(const Params& p, double now)
    {
        m_params = p;
        m_running = true;
        m_start = now;
        m_end = now + p.seconds;
        m_voters.clear();
    }

    void stop()
    {
        m_running = false;
        m_voters.clear();
    }

    bool eligibleTeam(int team) const
    {
        return (team == 1 || team == 2) && (m_params.team == 0 || team == m_params.team);
    }

    // Once per tick: beginSync, present() for every human player on a voting team, endSync. A voter
    // who is gone, or who left the team a team vote is for, loses the ballot.
    void beginSync()
    {
        for (std::map<int, Voter>::iterator it = m_voters.begin(); it != m_voters.end(); ++it)
            it->second.present = false;
    }

    // ping in seconds. A voter who switches team in a vote for everyone keeps the ballot and the box.
    void present(int id, int team, double ping)
    {
        Voter& v = m_voters[id];
        v.team = team;
        v.present = true;
        v.margin = ping > 0.0 ? ping : 0.0;
    }

    void endSync()
    {
        for (std::map<int, Voter>::iterator it = m_voters.begin(); it != m_voters.end();)
        {
            if (!it->second.present)
                m_voters.erase(it++);
            else
                ++it;
        }
    }

    bool isVoter(int id) const { return m_voters.count(id) != 0; }

    // A row reached one player's box, every player's, or one team's.
    void row(int id, double now)
    {
        std::map<int, Voter>::iterator it = m_voters.find(id);
        if (it != m_voters.end())
            addRow(it->second, now);
    }

    void rowAll(double now)
    {
        for (std::map<int, Voter>::iterator it = m_voters.begin(); it != m_voters.end(); ++it)
            addRow(it->second, now);
    }

    void rowTeam(int team, double now)
    {
        for (std::map<int, Voter>::iterator it = m_voters.begin(); it != m_voters.end(); ++it)
            if (it->second.team == team)
                addRow(it->second, now);
    }

    // The vote line was just posted to id. seen is false when the engine did not deliver the line
    // to the row counter while posting it: the line then still arrives later and is not counted.
    void posted(int id, double now, bool seen)
    {
        std::map<int, Voter>::iterator it = m_voters.find(id);
        if (it == m_voters.end())
            return;
        it->second.posted = true;
        it->second.rows = seen ? 0 : -1;
        it->second.lastRow = now;
    }

    // The voters whose vote line was never posted or is gone: pushed out by kBoxRows newer rows, or
    // cleared with the whole box kBoxSeconds after its last row.
    std::vector<int> due(double now) const
    {
        std::vector<int> out;
        for (std::map<int, Voter>::const_iterator it = m_voters.begin(); it != m_voters.end(); ++it)
        {
            const Voter& v = it->second;
            if (!v.posted || v.rows >= kBoxRows || now - v.lastRow >= kBoxSeconds + kClearMargin + v.margin)
                out.push_back(it->first);
        }
        return out;
    }

    // @return True when the ballot counts: a running vote and a voter. A voter may change their mind.
    bool ballot(int id, int choice)
    {
        if (!m_running || (choice != ChoiceYes && choice != ChoiceNo))
            return false;
        std::map<int, Voter>::iterator it = m_voters.find(id);
        if (it == m_voters.end())
            return false;
        it->second.choice = choice;
        return true;
    }

    void counts(int& yes, int& no, int& eligible) const
    {
        yes = no = 0;
        eligible = static_cast<int>(m_voters.size());
        for (std::map<int, Voter>::const_iterator it = m_voters.begin(); it != m_voters.end(); ++it)
        {
            if (it->second.choice == ChoiceYes)
                ++yes;
            else if (it->second.choice == ChoiceNo)
                ++no;
        }
    }

    // The result of the ballots so far.
    int verdict() const
    {
        int yes, no, eligible;
        counts(yes, no, eligible);
        const int cast = yes + no;
        const int needed = m_params.minVotes > 1 ? m_params.minVotes : 1;
        return cast >= needed && yes * 100 >= m_params.percent * cast ? OutcomePassed : OutcomeFailed;
    }

    // OutcomeNone while the vote runs; the verdict when the time is up or every voter has voted.
    int check(double now) const
    {
        if (!m_running)
            return OutcomeNone;
        int yes, no, eligible;
        counts(yes, no, eligible);
        if (now >= m_end || (eligible > 0 && yes + no == eligible))
            return verdict();
        return OutcomeNone;
    }

    int secondsLeft(double now) const
    {
        const double left = m_end - now;
        return left > 0.0 ? static_cast<int>(left + 0.999) : 0;
    }

    // The vote line: "VOTE: <text> (PgUp yes / PgDn no, 25 s left)".
    std::string line(double now) const
    {
        char tail[64];
        std::snprintf(tail, sizeof(tail), " (PgUp yes / PgDn no, %d s left)", secondsLeft(now));
        return "VOTE: " + m_params.text + tail;
    }

    // The voter's own confirmation: "<text> - you voted YES".
    std::string choiceLine(int choice) const
    {
        return m_params.text + (choice == ChoiceYes ? " - you voted YES" : " - you voted NO");
    }

    std::string resultLine(int outcome) const
    {
        int yes, no, eligible;
        counts(yes, no, eligible);
        char tail[64];
        std::snprintf(tail, sizeof(tail), " (yes %d, no %d)", yes, no);
        switch (outcome)
        {
        case OutcomePassed: return "VOTE PASSED: " + m_params.text + tail;
        case OutcomeFailed: return "VOTE FAILED: " + m_params.text + tail;
        default: return "VOTE CANCELLED: " + m_params.text;
        }
    }

private:
    struct Voter
    {
        int team;
        double margin;    // ping, seconds
        bool posted;
        int rows;         // rows after the vote line; -1 while the posted line is still on its way
        double lastRow;   // when the last row reached the box
        int choice;
        bool present;
        Voter() : team(0), margin(0.0), posted(false), rows(0), lastRow(0.0), choice(ChoiceNone), present(true) {}
    };

    static void addRow(Voter& v, double now)
    {
        if (!v.posted)
            return;
        ++v.rows;
        v.lastRow = now;
    }

    Params m_params;
    bool m_running;
    double m_start, m_end;
    std::map<int, Voter> m_voters;
};

} // namespace vote
} // namespace serverext

#endif // SERVEREXT_VOTE_H
