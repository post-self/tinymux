/*! \file comsys.cpp
 * \brief Channel Communication System.
 *
 * The functions here manage channels, channel membership, the comsys.db, and
 * the interaction of players and other objects with channels.
 */

#include "copyright.h"
#include "autoconf.h"
#include "config.h"
#include "externs.h"

using namespace std;

static int num_channels;
static comsys_t* comsys_table[NUM_COMSYS];

#define DFLT_MAX_LOG        0
#define MIN_RECALL_REQUEST  1
#define DFLT_RECALL_REQUEST 10
#define MAX_RECALL_REQUEST  200

// Return value is a static buffer.
//
static UTF8* RestrictTitleValue(const UTF8* pTitleRequest)
{
    // Remove all '\r\n\t' from the string.
    // Terminate any ANSI in the string.
    //
    static UTF8 NewTitle[MAX_TITLE_LEN + 1];
    StripTabsAndTruncate(pTitleRequest, NewTitle, MAX_TITLE_LEN, MAX_TITLE_LEN);
    return NewTitle;
}

static void do_setcomtitlestatus(const dbref player, struct channel* ch, const bool status)
{
    struct comuser* user = select_user(ch, player);
    if (ch && user)
    {
        user->ComTitleStatus = status;
    }
}

static void do_setnewtitle(const dbref player, struct channel* ch, const UTF8* pValidatedTitle)
{
    struct comuser* user = select_user(ch, player);

    if (ch && user)
    {
        if (user->title)
        {
            MEMFREE(user->title);
            user->title = nullptr;
        }
        user->title = StringClone(pValidatedTitle);
    }
}

// Save communication system data to disk.
//
void save_comsys(UTF8* filename)
{
    UTF8 buffer[500];

    mux_sprintf(buffer, sizeof(buffer), T("%s.#"), filename);
    FILE* fp;
    if (!mux_fopen(&fp, buffer, T("wb")))
    {
        Log.tinyprintf(T("Unable to open %s for writing." ENDLINE), buffer);
        return;
    }
    DebugTotalFiles++;

    mux_fprintf(fp, T("+V4\n"));
    mux_fprintf(fp, T("*** Begin CHANNELS ***\n"));

    save_channels(fp);

    mux_fprintf(fp, T("*** Begin COMSYS ***\n"));
    save_comsystem(fp);

    if (fclose(fp) == 0)
    {
        DebugTotalFiles--;
    }

    ReplaceFile(buffer, filename);
}

// Aliases must be between 1 and ALIAS_SIZE characters. No spaces. No ANSI.
//
UTF8* MakeCanonicalComAlias
(
    const UTF8* pAlias,
    size_t* nValidAlias,
    bool* bValidAlias
)
{
    static UTF8 Buffer[ALIAS_SIZE];
    *nValidAlias = 0;
    *bValidAlias = false;

    if (!pAlias)
    {
        return nullptr;
    }
    size_t n = 0;
    while (pAlias[n])
    {
        if (!mux_isprint_ascii(pAlias[n])
            || ' ' == pAlias[n])
        {
            return nullptr;
        }
        n++;
    }

    if (n < 1)
    {
        return nullptr;
    }
    if (MAX_ALIAS_LEN < n)
    {
        n = MAX_ALIAS_LEN;
    }
    memcpy(Buffer, pAlias, n);
    Buffer[n] = '\0';
    *nValidAlias = n;
    *bValidAlias = true;
    return Buffer;
}

static bool ParseChannelLine(UTF8* pBuffer, UTF8* pAlias5, UTF8** ppChannelName)
{
    // Fetch alias portion. We need to find the first space.
    //
    auto p = reinterpret_cast<UTF8*>(strchr(reinterpret_cast<char*>(pBuffer), ' '));
    if (!p)
    {
        return false;
    }

    *p = '\0';
    bool bValidAlias;
    size_t nValidAlias;
    const UTF8* pValidAlias = MakeCanonicalComAlias(pBuffer, &nValidAlias, &bValidAlias);
    if (!bValidAlias)
    {
        return false;
    }
    mux_strncpy(pAlias5, pValidAlias, ALIAS_SIZE - 1);

    // Skip any leading space before the channel name.
    //
    p++;
    while (mux_isspace(*p))
    {
        p++;
    }

    if (*p == '\0')
    {
        return false;
    }

    // The rest of the line is the channel name.
    //
    *ppChannelName = StringClone(p);
    return true;
}

static bool ReadListOfNumbers(FILE* fp, const int cnt, int anum[])
{
    UTF8 buffer[200];
    if (fgets(reinterpret_cast<char*>(buffer), sizeof(buffer), fp))
    {
        UTF8* p = buffer;
        for (int i = 0; i < cnt; i++)
        {
            if (mux_isdigit(p[0])
                || ('-' == p[0]
                    && mux_isdigit(p[1])))
            {
                anum[i] = mux_atol(p);
                do
                {
                    p++;
                }
                while (mux_isdigit(*p));

                if (' ' == *p)
                {
                    p++;
                }
            }
            else
            {
                return false;
            }
        }

        if ('\n' == *p)
        {
            return true;
        }
    }
    return false;
}


// Perform cleanup of comsystem data for players with 0 channels or player
// objects that were destroyed.
//
void purge_comsystem(void)
{
#ifdef ABORT_PURGE_COMSYS
    return;
#endif // ABORT_PURGE_COMSYS

    for (auto c : comsys_table)
    {
        while (c)
        {
            const comsys_t* d = c;
            c = c->next;
            if (d->numchannels == 0)
            {
                del_comsys(d->who);
                continue;
            }
            if (isPlayer(d->who))
            {
                continue;
            }
            if (God(Owner(d->who))
                && Going(d->who))
            {
                del_comsys(d->who);
            }
        }
    }
}

// Save Comsys channel data to the indicated file.
//
void save_channels(FILE* fp)
{
    purge_comsystem();

    comsys_t* c;
    int i, j;
    int np = 0;
    for (i = 0; i < NUM_COMSYS; i++)
    {
        c = comsys_table[i];

        while (c)
        {
            np++;
            c = c->next;
        }
    }

    mux_fprintf(fp, T("%d\n"), np);
    for (i = 0; i < NUM_COMSYS; i++)
    {
        c = comsys_table[i];

        while (c)
        {
            // Write user dbref and # of channels.
            //
            mux_fprintf(fp, T("%d %d\n"), c->who, c->numchannels);
            for (j = 0; j < c->numchannels; j++)
            {
                // Write channel alias and channel name.
                //
                mux_fprintf(fp, T("%s %s\n"), c->alias + j * ALIAS_SIZE, c->channels[j]);
            }
            c = c->next;
        }
    }
}

// Allocate and initialize new comsys_t struct.
//
comsys_t* create_new_comsys(void)
{
    auto c = static_cast<comsys_t*>(MEMALLOC(sizeof(comsys_t)));
    if (nullptr != c)
    {
        c->who = NOTHING;
        c->numchannels = 0;
        c->maxchannels = 0;
        c->alias = nullptr;
        c->channels = nullptr;
        c->next = nullptr;
    }
    else
    {
        ISOUTOFMEMORY(c);
    }
    return c;
}

static comsys_t* get_comsys(const dbref which)
{
    if (which < 0)
    {
        return nullptr;
    }

    comsys_t* c = comsys_table[which % NUM_COMSYS];

    while (c && (c->who != which))
    {
        c = c->next;
    }

    if (!c)
    {
        c = create_new_comsys();
        c->who = which;
        add_comsys(c);
    }

    return c;
}

// Insert comsys_t structure into the comsys_table.
//
void add_comsys(comsys_t* c)
{
    if (c->who < 0 || c->who >= mudstate.db_top)
    {
        Log.tinyprintf(T("add_comsys: dbref %d out of range [0, %d)" ENDLINE),
                       c->who, mudstate.db_top);
        return;
    }

    c->next = comsys_table[c->who % NUM_COMSYS];
    comsys_table[c->who % NUM_COMSYS] = c;
}

// Purge data for the given dbref from within the system.
//
void del_comsys(const dbref who)
{
    if (who < 0 || who >= mudstate.db_top)
    {
        Log.tinyprintf(T("del_comsys: dbref %d out of range [0, %d)" ENDLINE),
                       who, mudstate.db_top);
        return;
    }

    comsys_t* c = comsys_table[who % NUM_COMSYS];

    if (c == nullptr)
    {
        return;
    }

    if (c->who == who)
    {
        comsys_table[who % NUM_COMSYS] = c->next;
        destroy_comsys(c);
        return;
    }

    comsys_t* last = c;
    c = c->next;
    while (c)
    {
        if (c->who == who)
        {
            last->next = c->next;
            destroy_comsys(c);
            return;
        }
        last = c;
        c = c->next;
    }
}

// Deallocate memory for a particular comsys_t entry.
//
void destroy_comsys(comsys_t* c)
{
    if (c->alias)
    {
        MEMFREE(c->alias);
        c->alias = nullptr;
    }
    for (int i = 0; i < c->numchannels; i++)
    {
        MEMFREE(c->channels[i]);
        c->channels[i] = nullptr;
    }
    if (c->channels)
    {
        MEMFREE(c->channels);
        c->channels = nullptr;
    }
    MEMFREE(c);
    c = nullptr;
}

// Sort aliases.
//
void sort_com_aliases(comsys_t* c)
{
    bool cont = true;

    while (cont)
    {
        cont = false;
        for (int i = 0; i < c->numchannels - 1; i++)
        {
            if (strcmp(reinterpret_cast<char*>(c->alias) + i * ALIAS_SIZE,
                       reinterpret_cast<char*>(c->alias) + (i + 1) * ALIAS_SIZE) > 0)
            {
                UTF8 buffer[10];
                mux_strncpy(buffer, c->alias + i * ALIAS_SIZE, sizeof(buffer) - 1);
                mux_strncpy(c->alias + i * ALIAS_SIZE, c->alias + (i + 1) * ALIAS_SIZE, ALIAS_SIZE - 1);
                mux_strncpy(c->alias + (i + 1) * ALIAS_SIZE, buffer, ALIAS_SIZE - 1);
                UTF8* s = c->channels[i];
                c->channels[i] = c->channels[i + 1];
                c->channels[i + 1] = s;
                cont = true;
            }
        }
    }
}

// Lookup player's comsys data and find the channel associated with
// the given alias.
//
static UTF8* get_channel_from_alias(dbref player, UTF8* alias)
{
    int first;

    const comsys_t* c = get_comsys(player);

    int current = first = 0;
    int last = c->numchannels - 1;
    int dir = 1;

    while (dir && (first <= last))
    {
        current = (first + last) / 2;
        dir = strcmp(reinterpret_cast<char*>(alias), reinterpret_cast<char*>(c->alias) + ALIAS_SIZE * current);
        if (dir < 0)
            last = current - 1;
        else
            first = current + 1;
    }

    if (!dir)
    {
        return c->channels[current];
    }
    return (UTF8*)"";
}

// Version 4 start on 2007-MAR-17
//
//   -- Supports UTF-8 and ANSI as code-points.
//   -- Relies on a version number at the top of the file instead of within
//      this section.
//
void load_comsystem_V4(FILE* fp)
{
    UTF8 temp[LBUF_SIZE];

    num_channels = 0;

    int nc = 0;
    if (nullptr == fgets(reinterpret_cast<char*>(temp), sizeof(temp), fp))
    {
        return;
    }
    nc = mux_atol(temp);

    num_channels = nc;

    for (int i = 0; i < nc; i++)
    {
        int anum[10];

        auto ch = static_cast<channel*>(MEMALLOC(sizeof(struct channel)));
        ISOUTOFMEMORY(ch);

        size_t nChannel = GetLineTrunc(temp, sizeof(temp), fp);
        if (nChannel > MAX_CHANNEL_LEN)
        {
            nChannel = MAX_CHANNEL_LEN;
        }
        if (temp[nChannel - 1] == '\n')
        {
            // Get rid of trailing '\n'.
            //
            nChannel--;
        }
        memcpy(ch->name, temp, nChannel);
        ch->name[nChannel] = '\0';

        size_t nHeader = GetLineTrunc(temp, sizeof(temp), fp);
        if (nHeader > MAX_HEADER_LEN)
        {
            nHeader = MAX_HEADER_LEN;
        }
        if (temp[nHeader - 1] == '\n')
        {
            nHeader--;
        }
        memcpy(ch->header, temp, nHeader);
        ch->header[nHeader] = '\0';

        ch->on_users = nullptr;

        vector<UTF8> channel_name_vector(ch->name, ch->name+nChannel);
        mudstate.channel_names.insert(make_pair(channel_name_vector, ch));

        ch->type = 127;
        ch->temp1 = 0;
        ch->temp2 = 0;
        ch->charge = 0;
        ch->charge_who = NOTHING;
        ch->amount_col = 0;
        ch->num_messages = 0;
        ch->chan_obj = NOTHING;

        mux_assert(ReadListOfNumbers(fp, 8, anum));
        ch->type = anum[0];
        ch->temp1 = anum[1];
        ch->temp2 = anum[2];
        ch->charge = anum[3];
        ch->charge_who = anum[4];
        ch->amount_col = anum[5];
        ch->num_messages = anum[6];
        ch->chan_obj = anum[7];

        ch->num_users = 0;
        mux_assert(ReadListOfNumbers(fp, 1, &(ch->num_users)));
        ch->max_users = ch->num_users;
        if (ch->num_users > 0)
        {
            ch->users = static_cast<comuser**>(calloc(ch->max_users, sizeof(struct comuser*)));
            ISOUTOFMEMORY(ch->users);

            int jAdded = 0;
            for (int j = 0; j < ch->num_users; j++)
            {
                struct comuser t_user = {};

                t_user.who = NOTHING;
                t_user.bUserIsOn = false;
                t_user.ComTitleStatus = false;

                mux_assert(ReadListOfNumbers(fp, 3, anum));
                t_user.who = anum[0];
                t_user.bUserIsOn = (anum[1] ? true : false);
                t_user.ComTitleStatus = (anum[2] ? true : false);

                // Read Comtitle.
                //
                size_t nTitle = GetLineTrunc(temp, sizeof(temp), fp);
                const UTF8* pTitle = temp;

                if (!Good_dbref(t_user.who))
                {
                    Log.tinyprintf(
                        T("load_comsystem: dbref %d out of range [0, %d)." ENDLINE), t_user.who, mudstate.db_top);
                }
                else if (isGarbage(t_user.who))
                {
                    Log.tinyprintf(T("load_comsystem: dbref is GARBAGE." ENDLINE), t_user.who);
                }
                else
                {
                    // Validate comtitle.
                    //
                    if (3 < nTitle && temp[0] == 't' && temp[1] == ':')
                    {
                        pTitle = temp + 2;
                        nTitle -= 2;
                        if (pTitle[nTitle - 1] == '\n')
                        {
                            // Get rid of trailing '\n'.
                            //
                            nTitle--;
                        }
                        if (nTitle <= 0 || MAX_TITLE_LEN < nTitle)
                        {
                            nTitle = 0;
                            pTitle = temp;
                        }
                    }
                    else
                    {
                        nTitle = 0;
                    }

                    auto* user = static_cast<comuser*>(MEMALLOC(sizeof(struct comuser)));
                    ISOUTOFMEMORY(user);
                    memcpy(user, &t_user, sizeof(struct comuser));

                    user->title = StringCloneLen(pTitle, nTitle);
                    ch->users[jAdded++] = user;

                    if (!(isPlayer(user->who))
                        && !(Going(user->who)
                            && (God(Owner(user->who)))))
                    {
                        do_joinchannel(user->who, ch);
                    }
                    user->on_next = ch->on_users;
                    ch->on_users = user;
                }
            }
            ch->num_users = jAdded;
            sort_users(ch);
        }
        else
        {
            ch->users = nullptr;
        }
    }
}

// Load comsys database types 0, 1, 2 or 3.
//  Used Prior to Version 4 (2007-MAR-17)
//
void load_comsystem_V0123(FILE* fp)
{
    int ver = 0;
    UTF8 temp[LBUF_SIZE];

    num_channels = 0;

    int nc = 0;
    if (nullptr == fgets((char*)temp, sizeof(temp), fp))
    {
        return;
    }
    if (!strncmp((char*)temp, "+V", 2))
    {
        // +V2 has colored headers.
        //
        ver = mux_atol(temp + 2);
        if (ver < 1 || 3 < ver)
        {
            return;
        }

        mux_assert(ReadListOfNumbers(fp, 1, &nc));
    }
    else
    {
        nc = mux_atol(temp);
    }

    num_channels = nc;

    for (int i = 0; i < nc; i++)
    {
        int anum[10];

        auto ch = static_cast<channel*>(MEMALLOC(sizeof(struct channel)));
        ISOUTOFMEMORY(ch);

        size_t nChannel = GetLineTrunc(temp, sizeof(temp), fp);
        if (temp[nChannel - 1] == '\n')
        {
            // Get rid of trailing '\n'.
            //
            nChannel--;
            temp[nChannel] = '\0';
        }

        // Convert entire line to UTF-8 including ANSI escapes.
        //
        const UTF8* pBufferUnicode = ConvertToUTF8(reinterpret_cast<char*>(temp), &nChannel);
        if (MAX_CHANNEL_LEN < nChannel)
        {
            nChannel = MAX_CHANNEL_LEN;
            while (0 < nChannel
                && UTF8_CONTINUE <= utf8_FirstByte[temp[nChannel - 1]])
            {
                nChannel--;
            }
        }

        memcpy(ch->name, pBufferUnicode, nChannel);
        ch->name[nChannel] = '\0';

        if (ver >= 2)
        {
            size_t nHeader = GetLineTrunc(temp, sizeof(temp), fp);
            if (temp[nHeader - 1] == '\n')
            {
                nHeader--;
                temp[nHeader] = '\0';
            }

            // Convert entire line to UTF-8 including ANSI escapes.
            //
            pBufferUnicode = ConvertToUTF8(reinterpret_cast<char*>(temp), &nHeader);
            if (MAX_HEADER_LEN < nHeader)
            {
                nHeader = MAX_HEADER_LEN;
                while (0 < nHeader
                    && UTF8_CONTINUE <= utf8_FirstByte[static_cast<unsigned char>(pBufferUnicode[nHeader - 1])])
                {
                    nHeader--;
                }
            }

            memcpy(ch->header, pBufferUnicode, nHeader);
            ch->header[nHeader] = '\0';
        }

        ch->on_users = nullptr;

        vector<UTF8> channel_name_vector(ch->name, ch->name + nChannel);
        mudstate.channel_names.insert(make_pair(channel_name_vector, ch));

        ch->type = 127;
        ch->temp1 = 0;
        ch->temp2 = 0;
        ch->charge = 0;
        ch->charge_who = NOTHING;
        ch->amount_col = 0;
        ch->num_messages = 0;
        ch->chan_obj = NOTHING;

        if (ver >= 1)
        {
            mux_assert(ReadListOfNumbers(fp, 8, anum));
            ch->type = anum[0];
            ch->temp1 = anum[1];
            ch->temp2 = anum[2];
            ch->charge = anum[3];
            ch->charge_who = anum[4];
            ch->amount_col = anum[5];
            ch->num_messages = anum[6];
            ch->chan_obj = anum[7];
        }
        else
        {
            mux_assert(ReadListOfNumbers(fp, 10, anum));
            ch->type = anum[0];
            // anum[1] is not used.
            ch->temp1 = anum[2];
            ch->temp2 = anum[3];
            // anum[4] is not used.
            ch->charge = anum[5];
            ch->charge_who = anum[6];
            ch->amount_col = anum[7];
            ch->num_messages = anum[8];
            ch->chan_obj = anum[9];
        }

        if (ver <= 1)
        {
            // Build colored header if not +V2 or later db.
            //
            if (ch->type & CHANNEL_PUBLIC)
            {
                mux_sprintf(temp, sizeof(temp), T("%s[%s%s%s%s%s]%s"), COLOR_FG_CYAN, COLOR_INTENSE,
                            COLOR_FG_BLUE, ch->name, COLOR_RESET, COLOR_FG_CYAN, COLOR_RESET);
            }
            else
            {
                mux_sprintf(temp, sizeof(temp), T("%s[%s%s%s%s%s]%s"), COLOR_FG_MAGENTA, COLOR_INTENSE,
                            COLOR_FG_RED, ch->name, COLOR_RESET, COLOR_FG_MAGENTA,
                            COLOR_RESET);
            }
            StripTabsAndTruncate(temp, ch->header, MAX_HEADER_LEN, MAX_HEADER_LEN);
        }

        ch->num_users = 0;
        mux_assert(ReadListOfNumbers(fp, 1, &(ch->num_users)));
        ch->max_users = ch->num_users;
        if (ch->num_users > 0)
        {
            ch->users = static_cast<comuser**>(calloc(ch->max_users, sizeof(struct comuser*)));
            ISOUTOFMEMORY(ch->users);

            int jAdded = 0;
            for (int j = 0; j < ch->num_users; j++)
            {
                struct comuser t_user = {};

                t_user.who = NOTHING;
                t_user.bUserIsOn = false;
                t_user.ComTitleStatus = false;

                if (ver == 3)
                {
                    mux_assert(ReadListOfNumbers(fp, 3, anum));
                    t_user.who = anum[0];
                    t_user.bUserIsOn = (anum[1] ? true : false);
                    t_user.ComTitleStatus = (anum[2] ? true : false);
                }
                else
                {
                    t_user.ComTitleStatus = true;
                    if (ver)
                    {
                        mux_assert(ReadListOfNumbers(fp, 2, anum));
                        t_user.who = anum[0];
                        t_user.bUserIsOn = (anum[1] ? true : false);
                    }
                    else
                    {
                        mux_assert(ReadListOfNumbers(fp, 4, anum));
                        t_user.who = anum[0];
                        // anum[1] is not used.
                        // anum[2] is not used.
                        t_user.bUserIsOn = (anum[3] ? true : false);
                    }
                }

                // Read Comtitle.
                //
                size_t nTitle = GetLineTrunc(temp, sizeof(temp), fp);

                // Convert entire line to UTF-8 including ANSI escapes.
                //
                UTF8* pTitle = ConvertToUTF8(reinterpret_cast<char*>(temp), &nTitle);

                if (!Good_dbref(t_user.who))
                {
                    Log.tinyprintf(
                        T("load_comsystem: dbref %d out of range [0, %d)." ENDLINE), t_user.who, mudstate.db_top);
                }
                else if (isGarbage(t_user.who))
                {
                    Log.tinyprintf(T("load_comsystem: dbref is GARBAGE." ENDLINE), t_user.who);
                }
                else
                {
                    // Validate comtitle.
                    //
                    if (3 < nTitle && pTitle[0] == 't' && pTitle[1] == ':')
                    {
                        pTitle = pTitle + 2;
                        nTitle -= 2;
                        if (pTitle[nTitle - 1] == '\n')
                        {
                            // Get rid of trailing '\n'.
                            //
                            nTitle--;
                        }

                        if (nTitle <= 0
                            || MAX_TITLE_LEN < nTitle)
                        {
                            nTitle = 0;
                            pTitle = temp;
                        }
                    }
                    else
                    {
                        nTitle = 0;
                    }

                    const auto user = static_cast<comuser*>(MEMALLOC(sizeof(struct comuser)));
                    ISOUTOFMEMORY(user);
                    memcpy(user, &t_user, sizeof(struct comuser));

                    user->title = StringCloneLen(pTitle, nTitle);
                    ch->users[jAdded++] = user;

                    if (!(isPlayer(user->who))
                        && !(Going(user->who)
                            && (God(Owner(user->who)))))
                    {
                        do_joinchannel(user->who, ch);
                    }
                    user->on_next = ch->on_users;
                    ch->on_users = user;
                }
            }
            ch->num_users = jAdded;
            sort_users(ch);
        }
        else
        {
            ch->users = nullptr;
        }
    }
}

void load_channels_V4(FILE* fp)
{
    UTF8 buffer[LBUF_SIZE];

    int np = 0;
    mux_assert(ReadListOfNumbers(fp, 1, &np));
    for (int i = 0; i < np; i++)
    {
        int anum[2];
        comsys_t* c = create_new_comsys();
        c->who = 0;
        c->numchannels = 0;
        mux_assert(ReadListOfNumbers(fp, 2, anum));
        c->who = anum[0];
        c->numchannels = anum[1];
        c->maxchannels = c->numchannels;
        if (c->maxchannels > 0)
        {
            c->alias = static_cast<UTF8*>(MEMALLOC(c->maxchannels * ALIAS_SIZE));
            ISOUTOFMEMORY(c->alias);
            c->channels = static_cast<UTF8**>(MEMALLOC(sizeof(UTF8 *) * c->maxchannels));
            ISOUTOFMEMORY(c->channels);

            for (int j = 0; j < c->numchannels; j++)
            {
                size_t n = GetLineTrunc(buffer, sizeof(buffer), fp);
                if (buffer[n - 1] == '\n')
                {
                    // Get rid of trailing '\n'.
                    //
                    n--;
                    buffer[n] = '\0';
                }
                if (!ParseChannelLine(buffer, c->alias + j * ALIAS_SIZE, c->channels + j))
                {
                    c->numchannels--;
                    j--;
                }
            }
            sort_com_aliases(c);
        }
        else
        {
            c->alias = nullptr;
            c->channels = nullptr;
        }
        if (Good_obj(c->who))
        {
            add_comsys(c);
        }
        else
        {
            Log.tinyprintf(T("Invalid dbref %d." ENDLINE), c->who);
        }
        purge_comsystem();
    }
}

void load_channels_V0123(FILE* fp)
{
    char buffer[LBUF_SIZE];

    int np = 0;
    mux_assert(ReadListOfNumbers(fp, 1, &np));
    for (int i = 0; i < np; i++)
    {
        int anum[2];
        comsys_t* c = create_new_comsys();
        c->who = 0;
        c->numchannels = 0;
        mux_assert(ReadListOfNumbers(fp, 2, anum));
        c->who = anum[0];
        c->numchannels = anum[1];
        c->maxchannels = c->numchannels;
        if (c->maxchannels > 0)
        {
            c->alias = static_cast<UTF8*>(MEMALLOC(c->maxchannels * ALIAS_SIZE));
            ISOUTOFMEMORY(c->alias);
            c->channels = static_cast<UTF8**>(MEMALLOC(sizeof(UTF8 *) * c->maxchannels));
            ISOUTOFMEMORY(c->channels);

            for (int j = 0; j < c->numchannels; j++)
            {
                size_t n = GetLineTrunc(reinterpret_cast<UTF8*>(buffer), sizeof(buffer), fp);
                if (buffer[n - 1] == '\n')
                {
                    // Get rid of trailing '\n'.
                    //
                    n--;
                    buffer[n] = '\0';
                }

                // Convert the entire line to UTF8 before parsing the line
                // into fields.  The first field no ANSI in it anyway.
                //
                size_t nBufferUnicode;
                UTF8* pBufferUnicode = ConvertToUTF8(buffer, &nBufferUnicode);
                if (!ParseChannelLine(pBufferUnicode, c->alias + j * ALIAS_SIZE, c->channels + j))
                {
                    c->numchannels--;
                    j--;
                }
            }
            sort_com_aliases(c);
        }
        else
        {
            c->alias = nullptr;
            c->channels = nullptr;
        }
        if (Good_obj(c->who))
        {
            add_comsys(c);
        }
        else
        {
            Log.tinyprintf(T("Invalid dbref %d." ENDLINE), c->who);
        }
        purge_comsystem();
    }
}

void load_comsys_V4(FILE* fp)
{
    char buffer[200];
    if (fgets(buffer, sizeof(buffer), fp)
        && strcmp(buffer, "*** Begin CHANNELS ***\n") == 0)
    {
        load_channels_V4(fp);
    }
    else
    {
        Log.tinyprintf(T("Error: Couldn\xE2\x80\x99t find Begin CHANNELS." ENDLINE));
        return;
    }

    if (fgets(buffer, sizeof(buffer), fp)
        && strcmp(buffer, "*** Begin COMSYS ***\n") == 0)
    {
        load_comsystem_V4(fp);
    }
    else
    {
        Log.tinyprintf(T("Error: Couldn\xE2\x80\x99t find Begin COMSYS." ENDLINE));
    }
}

void load_comsys_V0123(FILE* fp)
{
    char buffer[200];
    if (fgets(buffer, sizeof(buffer), fp)
        && strcmp(buffer, "*** Begin CHANNELS ***\n") == 0)
    {
        load_channels_V0123(fp);
    }
    else
    {
        Log.tinyprintf(T("Error: Couldn\xE2\x80\x99t find Begin CHANNELS." ENDLINE));
        return;
    }

    if (fgets(buffer, sizeof(buffer), fp)
        && strcmp(buffer, "*** Begin COMSYS ***\n") == 0)
    {
        load_comsystem_V0123(fp);
    }
    else
    {
        Log.tinyprintf(T("Error: Couldn\xE2\x80\x99t find Begin COMSYS." ENDLINE));
    }
}


// Open the given filename, check version, and attempt to read comsystem
// data as indicated by the version number at the head of the file.
//
void load_comsys(UTF8* filename)
{
    for (int i = 0; i < NUM_COMSYS; i++)
    {
        comsys_table[i] = nullptr;
    }

    FILE* fp;
    if (!mux_fopen(&fp, filename, T("rb")))
    {
        Log.tinyprintf(T("Error: Couldn\xE2\x80\x99t find %s." ENDLINE), filename);
    }
    else
    {
        DebugTotalFiles++;
        Log.tinyprintf(T("LOADING: %s" ENDLINE), filename);
        const int ch = getc(fp);
        if (EOF == ch)
        {
            Log.tinyprintf(T("Error: Couldn\xE2\x80\x99t read first byte."));
        }
        else
        {
            ungetc(ch, fp);
            if ('+' == ch)
            {
                // Version 4 or later.
                //
                UTF8 nbuf1[8];

                // Read the version number.
                //
                if (fgets(reinterpret_cast<char*>(nbuf1), sizeof(nbuf1), fp))
                {
                    if (strncmp(reinterpret_cast<char*>(nbuf1), "+V4", 3) == 0)
                    {
                        // Started v4 on 2007-MAR-13.
                        //
                        load_comsys_V4(fp);
                    }
                }
            }
            else
            {
                load_comsys_V0123(fp);
            }
        }

        if (fclose(fp) == 0)
        {
            DebugTotalFiles--;
        }

        Log.tinyprintf(T("LOADING: %s (done)" ENDLINE), filename);
    }
}

// Save channel data and some user state info on a per-channel basis.
//
void save_comsystem(FILE* fp)
{
    struct comuser* user;
    int j;

    // Number of channels.
    //
    mux_fprintf(fp, T("%d\n"), num_channels);
    for (auto it = mudstate.channel_names.begin(); it != mudstate.channel_names.end(); ++it)
    {
        const auto ch = it->second;

        // Channel name.
        //
        mux_fprintf(fp, T("%s\n"), ch->name);

        // Channel header.
        //
        mux_fprintf(fp, T("%s\n"), ch->header);

        mux_fprintf(fp, T("%d %d %d %d %d %d %d %d\n"), ch->type, ch->temp1,
                    ch->temp2, ch->charge, ch->charge_who, ch->amount_col,
                    ch->num_messages, ch->chan_obj);

        // Count the number of 'valid' users to dump.
        //
        int number_of_valid_users = 0;
        for (j = 0; j < ch->num_users; j++)
        {
            user = ch->users[j];
            if (user->who >= 0 && user->who < mudstate.db_top)
            {
                number_of_valid_users++;
            }
        }

        // Number of users on this channel.
        //
        mux_fprintf(fp, T("%d\n"), number_of_valid_users);
        for (j = 0; j < ch->num_users; j++)
        {
            user = ch->users[j];
            if (user->who >= 0 && user->who < mudstate.db_top)
            {
                user = ch->users[j];

                // Write user state: dbref, on flag, and comtitle status.
                //
                mux_fprintf(fp, T("%d %d %d\n"), user->who, user->bUserIsOn, user->ComTitleStatus);

                // Write user title data.
                //
                if (user->title[0] != '\0')
                {
                    mux_fprintf(fp, T("t:%s\n"), user->title);
                }
                else
                {
                    mux_fprintf(fp, T("t:\n"));
                }
            }
        }
    }
}

static void BuildChannelMessage
(
    const bool bSpoof,
    const UTF8* pHeader,
    const struct comuser* user,
    const dbref ch_obj,
    const UTF8* pPose,
    UTF8** messNormal,
    UTF8** messNoComtitle
)
{
    // Allocate necessary buffers.
    //
    *messNormal = alloc_lbuf("BCM.messNormal");
    *messNoComtitle = nullptr;
    if (!bSpoof)
    {
        *messNoComtitle = alloc_lbuf("BCM.messNoComtitle");
    }

    // Comtitle Check.
    //
    const bool hasComTitle = (user->title[0] != '\0');

    UTF8* mnptr = *messNormal; // Message without comtitle removal.
    UTF8* mncptr = *messNoComtitle; // Message with comtitle removal.

    safe_str(pHeader, *messNormal, &mnptr);
    safe_chr(' ', *messNormal, &mnptr);
    if (!bSpoof)
    {
        safe_str(pHeader, *messNoComtitle, &mncptr);
        safe_chr(' ', *messNoComtitle, &mncptr);
    }

    // Don't evaluate a title if there isn't one to parse or evaluation of
    // comtitles is disabled.  If they're set spoof, ComTitleStatus doesn't
    // matter.
    //
    if (hasComTitle && (user->ComTitleStatus || bSpoof))
    {
        if (mudconf.eval_comtitle)
        {
            // Evaluate the comtitle as code.
            //
            UTF8 TempToEval[LBUF_SIZE];
            mux_strncpy(TempToEval, user->title, sizeof(TempToEval) - 1);
            mux_exec(TempToEval, LBUF_SIZE - 1, *messNormal, &mnptr, user->who, user->who, user->who,
                     EV_FCHECK | EV_EVAL | EV_TOP, nullptr, 0);
        }
        else
        {
            safe_str(user->title, *messNormal, &mnptr);
        }
        if (!bSpoof)
        {
            safe_chr(' ', *messNormal, &mnptr);
            safe_str(Moniker(user->who), *messNormal, &mnptr);
            safe_str(Moniker(user->who), *messNoComtitle, &mncptr);
        }
    }
    else
    {
        safe_str(Moniker(user->who), *messNormal, &mnptr);
        if (!bSpoof)
        {
            safe_str(Moniker(user->who), *messNoComtitle, &mncptr);
        }
    }

    bool bChannelSayString = false;
    bool bChannelSpeechMod = false;

    if (Good_obj(ch_obj))
    {
        dbref aowner;
        int aflags;
        UTF8* test_attr = atr_get("BuildChannelMessage.1304", ch_obj,
                                  A_SAYSTRING, &aowner, &aflags);

        if ('\0' != test_attr[0])
        {
            bChannelSayString = true;
        }
        free_lbuf(test_attr);

        test_attr = atr_get("BuildChannelMessage.1312", ch_obj,
                            A_SPEECHMOD, &aowner, &aflags);

        if ('\0' != test_attr[0])
        {
            bChannelSpeechMod = true;
        }
        free_lbuf(test_attr);
    }

    UTF8* saystring = nullptr;
    UTF8* newPose = nullptr;

    char noSpaceChars[] = { '\'', '#', ':', '-', ',' };
    bool noSpaceCharFound = false;

    switch (pPose[0])
    {
    case ':':
        pPose++;
        while (pPose[0] == ' ')
        {
            pPose++;
        }
        for (int i = 0; i < sizeof noSpaceChars; i++)
        {
            if (pPose[0] == noSpaceChars[i])
            {
                noSpaceCharFound = true;
                break;
            }
        }
        newPose = modSpeech(bChannelSpeechMod ? ch_obj : user->who, pPose, true, T("channel/pose"));
        if (newPose)
        {
            pPose = newPose;
        }
        if (!noSpaceCharFound)
        {
            safe_chr(' ', *messNormal, &mnptr);
        }
        safe_str(pPose, *messNormal, &mnptr);
        if (!bSpoof)
        {
            safe_chr(' ', *messNoComtitle, &mncptr);
            safe_str(pPose, *messNoComtitle, &mncptr);
        }
        break;

    case ';':
        pPose++;
        newPose = modSpeech(bChannelSpeechMod ? ch_obj : user->who, pPose, true, T("channel/pose"));
        if (newPose)
        {
            pPose = newPose;
        }
        safe_str(pPose, *messNormal, &mnptr);
        if (!bSpoof)
        {
            safe_str(pPose, *messNoComtitle, &mncptr);
        }
        break;

    default:
        newPose = modSpeech(bChannelSpeechMod ? ch_obj : user->who, pPose, true, T("channel"));
        if (newPose)
        {
            pPose = newPose;
        }
        saystring = modSpeech(bChannelSayString ? ch_obj : user->who, pPose, false, T("channel"));
        if (saystring)
        {
            safe_chr(' ', *messNormal, &mnptr);
            safe_str(saystring, *messNormal, &mnptr);
            safe_str(T(" \""), *messNormal, &mnptr);
        }
        else
        {
            safe_str(T(" says, \""), *messNormal, &mnptr);
        }
        safe_str(pPose, *messNormal, &mnptr);
        safe_str(T("\""), *messNormal, &mnptr);
        if (!bSpoof)
        {
            if (saystring)
            {
                safe_chr(' ', *messNoComtitle, &mncptr);
                safe_str(saystring, *messNoComtitle, &mncptr);
                safe_str(T(" \""), *messNoComtitle, &mncptr);
            }
            else
            {
                safe_str(T(" says, \""), *messNoComtitle, &mncptr);
            }
            safe_str(pPose, *messNoComtitle, &mncptr);
            safe_str(T("\""), *messNoComtitle, &mncptr);
        }
        break;
    }
    *mnptr = '\0';
    if (!bSpoof)
    {
        *mncptr = '\0';
    }
    if (newPose)
    {
        free_lbuf(newPose);
    }
    if (saystring)
    {
        free_lbuf(saystring);
    }
}

static void do_processcom(dbref player, UTF8* arg1, UTF8* arg2)
{
    if (!*arg2)
    {
        raw_notify(player, T("No message."));
        return;
    }
    if (3500 < strlen(reinterpret_cast<const char*>(arg2)))
    {
        arg2[3500] = '\0';
    }
    struct channel* ch = select_channel(arg1);
    if (!ch)
    {
        raw_notify(player, tprintf(T("Unknown channel %s."), arg1));
        return;
    }
    struct comuser* user = select_user(ch, player);
    if (!user)
    {
        raw_notify(player, T("You are not listed as on that channel.  Delete this alias and readd."));
        return;
    }

    if (Gagged(player)
        && !Wizard(player))
    {
        raw_notify(player, T("GAGGED players may not speak on channels."));
        return;
    }

    if (!strcmp(reinterpret_cast<const char*>(arg2), "on"))
    {
        do_joinchannel(player, ch);
    }
    else if (!strcmp(reinterpret_cast<const char*>(arg2), "off"))
    {
        do_leavechannel(player, ch);
    }
    else if (!user->bUserIsOn)
    {
        raw_notify(player, tprintf(T("You must be on %s to do that."), arg1));
    }
    else if (!strcmp(reinterpret_cast<const char*>(arg2), "who"))
    {
        do_comwho(player, ch);
    }
    else if (!strncmp(reinterpret_cast<const char*>(arg2), "last", 4)
        && (arg2[4] == '\0'
            || (arg2[4] == ' '
                && is_integer(arg2 + 5, nullptr))))
    {
        // Parse optional number after the 'last' command.
        //
        int nRecall = DFLT_RECALL_REQUEST;
        if (arg2[4] == ' ')
        {
            nRecall = mux_atol(arg2 + 5);
        }
        do_comlast(player, ch, nRecall);
    }
    else if (!test_transmit_access(player, ch))
    {
        raw_notify(player, T("That channel type cannot be transmitted on."));
    }
    else
    {
        if (!payfor(player, Guest(player) ? 0 : ch->charge))
        {
            raw_notify(player, tprintf(T("You don\xE2\x80\x99t have enough %s."), mudconf.many_coins));
            return;
        }
        ch->amount_col += ch->charge;
        giveto(ch->charge_who, ch->charge);

        // BuildChannelMessage allocates messNormal and messNoComtitle,
        // SendChannelMessage frees them.
        //
        UTF8* messNormal;
        UTF8* messNoComtitle;
        BuildChannelMessage((ch->type & CHANNEL_SPOOF) != 0, ch->header, user,
                            ch->chan_obj, arg2, &messNormal, &messNoComtitle);
        SendChannelMessage(player, ch, messNormal, messNoComtitle);
    }
}

inline void notify_comsys(const dbref target, const dbref sender, const mux_string& msg)
{
    notify_with_cause_ooc(target, sender, msg, MSG_SRC_COMSYS);
}

// Transmit the given message as appropriate to all listening parties.
// Perform channel message logging, if configured, for the channel.
//
void SendChannelMessage
(
    const dbref executor,
    struct channel* ch,
    UTF8* msgNormal,
    UTF8* msgNoComtitle
)
{
    // Transmit messages.
    //
    const bool bSpoof = ((ch->type & CHANNEL_SPOOF) != 0);
    ch->num_messages++;

    for (struct comuser* user = ch->on_users; user; user = user->on_next)
    {
        if (user->bUserIsOn
            && test_receive_access(user->who, ch))
        {
            if (user->ComTitleStatus
                || bSpoof
                || msgNoComtitle == nullptr)
            {
                notify_comsys(user->who, executor, msgNormal);
            }
            else
            {
                notify_comsys(user->who, executor, msgNoComtitle);
            }
        }
    }

    // Handle logging.
    //
    const dbref obj = ch->chan_obj;
    if (Good_obj(obj))
    {
        dbref aowner;
        int aflags;
        int logmax = DFLT_MAX_LOG;
        ATTR* pattr = atr_str(T("MAX_LOG"));
        if (pattr
            && pattr->number)
        {
            UTF8* maxbuf = atr_get("SendChannelMessage.1141", obj, pattr->number, &aowner, &aflags);
            logmax = mux_atol(maxbuf);
            free_lbuf(maxbuf);
        }

        if (0 < logmax)
        {
            if (logmax > MAX_RECALL_REQUEST)
            {
                logmax = MAX_RECALL_REQUEST;
                atr_add(ch->chan_obj, pattr->number, mux_ltoa_t(logmax), GOD,
                        AF_CONST | AF_NOPROG | AF_NOPARSE);
            }
            const UTF8* p = tprintf(T("HISTORY_%d"), iMod(ch->num_messages, logmax));
            const int atr = mkattr(GOD, p);
            if (0 < atr)
            {
                pattr = atr_str(T("LOG_TIMESTAMPS"));
                if (pattr
                    && atr_get_info(obj, pattr->number, &aowner, &aflags))
                {
                    CLinearTimeAbsolute ltaNow;
                    ltaNow.GetLocal();

                    // Save message in history with timestamp.
                    //
                    UTF8 temp[LBUF_SIZE];
                    mux_sprintf(temp, sizeof(temp), T("[%s] %s"), ltaNow.ReturnDateString(0), msgNormal);
                    atr_add(ch->chan_obj, atr, temp, GOD, AF_CONST | AF_NOPROG | AF_NOPARSE);
                }
                else
                {
                    // Save message in history without timestamp.
                    //
                    atr_add(ch->chan_obj, atr, msgNormal, GOD,
                            AF_CONST | AF_NOPROG | AF_NOPARSE);
                }
            }
        }
    }
    else if (ch->chan_obj != NOTHING)
    {
        ch->chan_obj = NOTHING;
    }

    // Since msgNormal and msgNoComTitle are no longer needed, free them here.
    //
    if (msgNormal)
    {
        free_lbuf(msgNormal);
    }
    if (msgNoComtitle
        && msgNoComtitle != msgNormal)
    {
        free_lbuf(msgNoComtitle);
    }
}

static void ChannelMOTD(dbref executor, dbref enactor, int attr)
{
    if (Good_obj(executor))
    {
        dbref aowner;
        int aflags;
        UTF8* q = atr_get("ChannelMOTD.1186", executor, attr, &aowner, &aflags);
        if ('\0' != q[0])
        {
            UTF8* buf = alloc_lbuf("chanmotd");
            UTF8* bp = buf;

            mux_exec(q, LBUF_SIZE - 1, buf, &bp, executor, executor, enactor,
                     AttrTrace(aflags, EV_FCHECK|EV_EVAL|EV_TOP), nullptr, 0);
            *bp = '\0';

            notify_comsys(enactor, executor, buf);

            free_lbuf(buf);
        }
        free_lbuf(q);
    }
}


// Add player to the given channel.  Transmit join messages as appropriate.
//
void do_joinchannel(const dbref player, struct channel* ch)
{
    int attr;

    struct comuser* user = select_user(ch, player);

    if (!user)
    {
        int i;
        if (ch->num_users >= MAX_USERS_PER_CHANNEL)
        {
            raw_notify(player, tprintf(T("Too many people on channel %s already."),
                                       ch->name));
            return;
        }

        user = static_cast<comuser*>(MEMALLOC(sizeof(struct comuser)));
        if (nullptr == user)
        {
            raw_notify(player, OUT_OF_MEMORY);
            return;
        }

        ch->num_users++;
        if (ch->num_users >= ch->max_users)
        {
            struct comuser** cu;
            ch->max_users = ch->num_users + 10;
            cu = static_cast<comuser**>(MEMALLOC(sizeof(struct comuser *) * ch->max_users));
            ISOUTOFMEMORY(cu);

            for (i = 0; i < (ch->num_users - 1); i++)
            {
                cu[i] = ch->users[i];
            }
            MEMFREE(ch->users);
            ch->users = cu;
        }

        for (i = ch->num_users - 1; 0 < i && player < ch->users[i - 1]->who; --i)
        {
            ch->users[i] = ch->users[i - 1];
        }
        ch->users[i] = user;

        user->who = player;
        user->bUserIsOn = true;
        user->ComTitleStatus = true;
        user->title = StringClone(T(""));

        // if (Connected(player))&&(isPlayer(player))
        //
        if (UNDEAD(player))
        {
            user->on_next = ch->on_users;
            ch->on_users = user;
        }
        attr = A_COMJOIN;
    }
    else if (!user->bUserIsOn)
    {
        user->bUserIsOn = true;
        attr = A_COMON;
    }
    else
    {
        raw_notify(player, tprintf(T("You are already on channel %s."), ch->name));
        return;
    }

    if (!Hidden(player))
    {
        UTF8 *messNormal, *messNoComtitle;
        BuildChannelMessage((ch->type & CHANNEL_SPOOF) != 0, ch->header, user,
                            ch->chan_obj, T(":has joined this channel."), &messNormal,
                            &messNoComtitle);
        SendChannelMessage(player, ch, messNormal, messNoComtitle);
    }
    ChannelMOTD(ch->chan_obj, user->who, attr);
}

// Process leave channnel request.
//
void do_leavechannel(dbref player, struct channel* ch)
{
    struct comuser* user = select_user(ch, player);
    raw_notify(player, tprintf(T("You have left channel %s."), ch->name));

    if (user->bUserIsOn)
    {
        if (!Hidden(player))
        {
            UTF8 *messNormal, *messNoComtitle;
            BuildChannelMessage((ch->type & CHANNEL_SPOOF) != 0, ch->header, user,
                                ch->chan_obj, T(":has left this channel."), &messNormal,
                                &messNoComtitle);
            SendChannelMessage(player, ch, messNormal, messNoComtitle);
        }
        ChannelMOTD(ch->chan_obj, user->who, A_COMOFF);
        user->bUserIsOn = false;
    }
}

static void do_comwho_line
(
    const dbref player,
    struct channel* ch,
    const struct comuser* user
)
{
    UTF8* msg;
    UTF8* buff = nullptr;

    if (user->title[0] != '\0')
    {
        // There is a comtitle.
        //
        if (Staff(player))
        {
            buff = unparse_object(player, user->who, false);
            if (ch->type & CHANNEL_SPOOF)
            {
                msg = tprintf(T("%s as %s"), buff, user->title);
            }
            else
            {
                msg = tprintf(T("%s as %s %s"), buff, user->title, buff);
            }
        }
        else
        {
            if (ch->type & CHANNEL_SPOOF)
            {
                msg = user->title;
            }
            else
            {
                buff = unparse_object(player, user->who, false);
                msg = tprintf(T("%s %s"), user->title, buff);
            }
        }
    }
    else
    {
        buff = unparse_object(player, user->who, false);
        msg = buff;
    }

    raw_notify(player, msg);
    if (buff)
    {
        free_lbuf(buff);
    }
}

void do_comwho(dbref player, struct channel* ch)
{
    struct comuser* user;

    raw_notify(player, T("-- Players --"));
    for (user = ch->on_users; user; user = user->on_next)
    {
        if (isPlayer(user->who))
        {
            if (Connected(user->who)
                && (!Hidden(user->who)
                    || Wizard_Who(player)
                    || See_Hidden(player)))
            {
                if (user->bUserIsOn)
                {
                    do_comwho_line(player, ch, user);
                }
            }
            else if (!Hidden(user->who))
            {
                do_comdisconnectchannel(user->who, ch->name);
            }
        }
    }
    raw_notify(player, T("-- Objects --"));
    for (user = ch->on_users; user; user = user->on_next)
    {
        if (!isPlayer(user->who))
        {
            if (Going(user->who)
                && God(Owner(user->who)))
            {
                do_comdisconnectchannel(user->who, ch->name);
            }
            else if (user->bUserIsOn)
            {
                do_comwho_line(player, ch, user);
            }
        }
    }
    raw_notify(player, tprintf(T("-- %s --"), ch->name));
}

void do_comlast(dbref player, struct channel* ch, int arg)
{
    // Validate the channel object.
    //
    if (!Good_obj(ch->chan_obj))
    {
        raw_notify(player, T("Channel does not have an object."));
        return;
    }

    dbref aowner;
    int aflags;
    const dbref obj = ch->chan_obj;
    int logmax = MAX_RECALL_REQUEST;

    // Lookup depth of logging.
    //
    ATTR* pattr = atr_str(T("MAX_LOG"));
    if (pattr
        && (atr_get_info(obj, pattr->number, &aowner, &aflags)))
    {
        UTF8* maxbuf = atr_get("do_comlast.1408", obj, pattr->number, &aowner, &aflags);
        logmax = mux_atol(maxbuf);
        free_lbuf(maxbuf);
    }

    if (logmax < 1)
    {
        raw_notify(player, T("Channel does not log."));
        return;
    }

    if (arg < MIN_RECALL_REQUEST)
    {
        arg = MIN_RECALL_REQUEST;
    }

    if (arg > logmax)
    {
        arg = logmax;
    }

    int histnum = ch->num_messages - arg;

    raw_notify(player, tprintf(T("%s -- Begin Comsys Recall --"), ch->header));

    for (int count = 0; count < arg; count++)
    {
        histnum++;
        pattr = atr_str(tprintf(T("HISTORY_%d"), iMod(histnum, logmax)));
        if (pattr)
        {
            UTF8* message = atr_get("do_comlast.1436", obj, pattr->number,
                                    &aowner, &aflags);
            raw_notify(player, message);
            free_lbuf(message);
        }
    }

    raw_notify(player, tprintf(T("%s -- End Comsys Recall --"), ch->header));
}

// Turn channel history timestamping on or off for the given channel.
//
static bool do_chanlog_timestamps(dbref player, UTF8* channel, UTF8* arg)
{
    UNUSED_PARAMETER(player);

    // Validate arg.
    //
    int value = 0;
    if (nullptr == arg
        || !is_integer(arg, nullptr)
        || ((value = mux_atol(arg)) != 0
            && value != 1))
    {
        // arg is not "0" and not "1".
        //
        return false;
    }

    struct channel* ch = select_channel(channel);
    if (!Good_obj(ch->chan_obj))
    {
        // No channel object has been set.
        //
        return false;
    }

    dbref aowner;
    int aflags;
    ATTR* pattr = atr_str(T("MAX_LOG"));
    if (nullptr == pattr
        || !atr_get_info(ch->chan_obj, pattr->number, &aowner, &aflags))
    {
        // Logging isn't enabled.
        //
        return false;
    }

    const int atr = mkattr(GOD, T("LOG_TIMESTAMPS"));
    if (atr <= 0)
    {
        return false;
    }

    if (value)
    {
        atr_add(ch->chan_obj, atr, mux_ltoa_t(value), GOD,
                AF_CONST | AF_NOPROG | AF_NOPARSE);
    }
    else
    {
        atr_clr(ch->chan_obj, atr);
    }

    return true;
}

// Set number of entries for channel logging.
//
static bool do_chanlog(dbref player, UTF8* channel, UTF8* arg)
{
    UNUSED_PARAMETER(player);

    int value;
    if (!*arg
        || !is_integer(arg, nullptr)
        || (value = mux_atol(arg)) > MAX_RECALL_REQUEST)
    {
        return false;
    }

    if (value < 0)
    {
        value = 0;
    }

    const struct channel* ch = select_channel(channel);
    if (!Good_obj(ch->chan_obj))
    {
        // No channel object has been set.
        //
        return false;
    }

    const int atr = mkattr(GOD, T("MAX_LOG"));
    if (atr <= 0)
    {
        return false;
    }

    dbref aowner;
    int aflags;
    UTF8* oldvalue = atr_get("do_chanlog.1477", ch->chan_obj, atr, &aowner, &aflags);
    const int oldnum = mux_atol(oldvalue);
    if (value < oldnum)
    {
        for (int count = 0; count <= oldnum; count++)
        {
            ATTR* hist = atr_str(tprintf(T("HISTORY_%d"), count));
            if (hist)
            {
                atr_clr(ch->chan_obj, hist->number);
            }
        }
    }
    free_lbuf(oldvalue);
    atr_add(ch->chan_obj, atr, mux_ltoa_t(value), GOD,
            AF_CONST | AF_NOPROG | AF_NOPARSE);
    return true;
}

// Find struct channel entry by name with the channel_name hash table.
//
struct channel* select_channel(UTF8* channel_name)
{
    const auto channel_name_length = strlen(reinterpret_cast<char*>(channel_name));
    const vector<UTF8> channel_vector(channel_name, channel_name + channel_name_length);
    const auto it = mudstate.channel_names.find(channel_vector);
    if (it != mudstate.channel_names.end())
    {
        return it->second;
    }
    return nullptr;
}

// Locate player in the user's list for the given channel.
//
struct comuser* select_user(struct channel* ch, const dbref player)
{
    if (!ch)
    {
        return nullptr;
    }

    int first = 0;
    int last = ch->num_users - 1;
    int dir = 1;
    int current = 0;

    while (dir && (first <= last))
    {
        current = (first + last) / 2;
        if (ch->users[current] == nullptr)
        {
            last--;
            continue;
        }
        if (ch->users[current]->who == player)
        {
            dir = 0;
        }
        else if (ch->users[current]->who < player)
        {
            dir = 1;
            first = current + 1;
        }
        else
        {
            dir = -1;
            last = current - 1;
        }
    }

    if (!dir)
    {
        return ch->users[current];
    }
    return nullptr;
}

#define MAX_ALIASES_PER_PLAYER 100

void do_addcom
(
    const dbref executor,
    const dbref caller,
    dbref enactor,
    const int eval,
    const int key,
    const int nargs,
    UTF8* arg1,
    UTF8* channel,
    const UTF8* cargs[],
    int ncargs
)
{
    UNUSED_PARAMETER(caller);
    UNUSED_PARAMETER(enactor);
    UNUSED_PARAMETER(eval);
    UNUSED_PARAMETER(key);
    UNUSED_PARAMETER(nargs);
    UNUSED_PARAMETER(cargs);
    UNUSED_PARAMETER(ncargs);

    if (!mudconf.have_comsys)
    {
        raw_notify(executor, T("Comsys disabled."));
        return;
    }
    bool bValidAlias;
    size_t nValidAlias;
    UTF8* pValidAlias = MakeCanonicalComAlias(arg1, &nValidAlias, &bValidAlias);
    if (!bValidAlias)
    {
        raw_notify(executor, T("You need to specify a valid alias."));
        return;
    }
    if ('\0' == channel[0])
    {
        raw_notify(executor, T("You need to specify a channel."));
        return;
    }

    int i, j;
    struct channel* ch = select_channel(channel);
    if (!ch)
    {
        UTF8 Buffer[MAX_CHANNEL_LEN + 1];
        StripTabsAndTruncate(channel, Buffer, MAX_CHANNEL_LEN, MAX_CHANNEL_LEN);
        raw_notify(executor, tprintf(T("Channel %s does not exist yet."), Buffer));
        return;
    }
    if (!test_join_access(executor, ch))
    {
        raw_notify(executor, T("Sorry, this channel type does not allow you to join."));
        return;
    }
    comsys_t* c = get_comsys(executor);
    if (c->numchannels >= MAX_ALIASES_PER_PLAYER)
    {
        raw_notify(executor, tprintf(T("Sorry, but you have reached the maximum number of aliases allowed.")));
        return;
    }
    for (j = 0; j < c->numchannels && (strcmp(reinterpret_cast<char*>(pValidAlias),
                                              reinterpret_cast<char*>(c->alias) + j * ALIAS_SIZE) > 0); j++)
    {
    }
    if (j < c->numchannels && !strcmp(reinterpret_cast<char*>(pValidAlias),
                                      reinterpret_cast<char*>(c->alias) + j * ALIAS_SIZE))
    {
        const UTF8* p = tprintf(T("That alias is already in use for channel %s."), c->channels[j]);
        raw_notify(executor, p);
        return;
    }
    if (c->numchannels >= c->maxchannels)
    {
        c->maxchannels = c->numchannels + 10;

        const auto na = static_cast<UTF8*>(MEMALLOC(ALIAS_SIZE * c->maxchannels));
        ISOUTOFMEMORY(na);
        const auto nc = static_cast<UTF8**>(MEMALLOC(sizeof(UTF8 *) * c->maxchannels));
        ISOUTOFMEMORY(nc);

        for (i = 0; i < c->numchannels; i++)
        {
            mux_strncpy(na + i * ALIAS_SIZE, c->alias + i * ALIAS_SIZE, ALIAS_SIZE - 1);
            nc[i] = c->channels[i];
        }
        if (c->alias)
        {
            MEMFREE(c->alias);
            c->alias = nullptr;
        }
        if (c->channels)
        {
            MEMFREE(c->channels);
            c->channels = nullptr;
        }
        c->alias = na;
        c->channels = nc;
    }
    int where = c->numchannels++;
    for (i = where; i > j; i--)
    {
        mux_strncpy(c->alias + i * ALIAS_SIZE, c->alias + (i - 1) * ALIAS_SIZE, ALIAS_SIZE - 1);
        c->channels[i] = c->channels[i - 1];
    }

    where = j;
    memcpy(c->alias + where * ALIAS_SIZE, pValidAlias, nValidAlias);
    *(c->alias + where * ALIAS_SIZE + nValidAlias) = '\0';
    c->channels[where] = StringClone(channel);

    if (!select_user(ch, executor))
    {
        do_joinchannel(executor, ch);
    }

    raw_notify(executor, tprintf(T("Channel %s added with alias %s."), channel, pValidAlias));
}

void do_delcom(dbref executor, dbref caller, dbref enactor, int eval, int key, UTF8* arg1, const UTF8* cargs[],
               int ncargs)
{
    UNUSED_PARAMETER(caller);
    UNUSED_PARAMETER(enactor);
    UNUSED_PARAMETER(eval);
    UNUSED_PARAMETER(key);
    UNUSED_PARAMETER(cargs);
    UNUSED_PARAMETER(ncargs);

    if (!mudconf.have_comsys)
    {
        raw_notify(executor, T("Comsys disabled."));
        return;
    }
    if (!arg1)
    {
        raw_notify(executor, T("Need an alias to delete."));
        return;
    }
    comsys_t* c = get_comsys(executor);

    for (int i = 0; i < c->numchannels; i++)
    {
        if (!strcmp(reinterpret_cast<char*>(arg1), reinterpret_cast<char*>(c->alias) + i * ALIAS_SIZE))
        {
            int found = 0;
            for (int itmp = 0; itmp < c->numchannels; itmp++)
            {
                if (!strcmp(reinterpret_cast<char*>(c->channels[itmp]), reinterpret_cast<char*>(c->channels[i])))
                {
                    found++;
                }
            }

            // If we found no other channels, delete it.
            //
            if (found <= 1)
            {
                do_delcomchannel(executor, c->channels[i], false);
                raw_notify(executor, tprintf(T("Alias %s for channel %s deleted."),
                                             arg1, c->channels[i]));
                MEMFREE(c->channels[i]);
            }
            else
            {
                raw_notify(executor, tprintf(T("Alias %s for channel %s deleted."),
                                             arg1, c->channels[i]));
            }

            c->channels[i] = nullptr;
            c->numchannels--;

            for (; i < c->numchannels; i++)
            {
                mux_strncpy(c->alias + i * ALIAS_SIZE, c->alias + (i + 1) * ALIAS_SIZE, ALIAS_SIZE - 1);
                c->channels[i] = c->channels[i + 1];
            }
            return;
        }
    }
    raw_notify(executor, T("Unable to find that alias."));
}

// Process a complete unsubscribe for a player from a particular channel.
//
void do_delcomchannel(dbref player, UTF8* channel, bool bQuiet)
{
    struct channel* ch = select_channel(channel);
    if (!ch)
    {
        raw_notify(player, tprintf(T("Unknown channel %s."), channel));
    }
    else
    {
        int i;
        int j = 0;
        for (i = 0; i < ch->num_users && !j; i++)
        {
            struct comuser* user = ch->users[i];
            if (user->who == player)
            {
                do_comdisconnectchannel(player, channel);
                if (!bQuiet)
                {
                    if (user->bUserIsOn
                        && !Hidden(player))
                    {
                        UTF8 *messNormal, *messNoComtitle;
                        BuildChannelMessage((ch->type & CHANNEL_SPOOF) != 0,
                                            ch->header, user, ch->chan_obj,
                                            T(":has left this channel."), &messNormal,
                                            &messNoComtitle);
                        SendChannelMessage(player, ch, messNormal, messNoComtitle);
                    }
                    raw_notify(player, tprintf(T("You have left channel %s."),
                                               channel));
                }

                ChannelMOTD(ch->chan_obj, user->who, A_COMLEAVE);

                if (user->title)
                {
                    MEMFREE(user->title);
                    user->title = nullptr;
                }
                MEMFREE(user);
                user = nullptr;
                j = 1;
            }
        }

        if (j)
        {
            ch->num_users--;
            for (i--; i < ch->num_users; i++)
            {
                ch->users[i] = ch->users[i + 1];
            }
        }
    }
}

void do_createchannel(const dbref executor, const dbref caller, dbref enactor, const int eval, const int key, UTF8* channel,
                      const UTF8* cargs[], const int ncargs)
{
    UNUSED_PARAMETER(caller);
    UNUSED_PARAMETER(enactor);
    UNUSED_PARAMETER(eval);
    UNUSED_PARAMETER(key);
    UNUSED_PARAMETER(cargs);
    UNUSED_PARAMETER(ncargs);

    if ('\0' == channel[0])
    {
        raw_notify(executor, T("You must specify a channel to create."));
        return;
    }

    if (!Comm_All(executor))
    {
        raw_notify(executor, NOPERM_MESSAGE);
        return;
    }

    auto newchannel = static_cast<::channel*>(MEMALLOC(sizeof(struct channel)));
    if (nullptr == newchannel)
    {
        raw_notify(executor, T("Out of memory."));
        return;
    }

    size_t nNameNoANSI;
    UTF8* pNameNoANSI;
    UTF8 Buffer[MAX_HEADER_LEN + 1];
    mux_field fldChannel = StripTabsAndTruncate(channel, Buffer, MAX_HEADER_LEN,
                                                MAX_HEADER_LEN);
    if (fldChannel.m_byte == fldChannel.m_column)
    {
        // The channel name does not contain ANSI, so first, we add some to
        // get the header.
        //
        const size_t nMax = MAX_HEADER_LEN - (sizeof(COLOR_INTENSE) - 1)
            - (sizeof(COLOR_RESET) - 1) - 2;
        size_t nChannel = fldChannel.m_byte;
        if (nMax < nChannel)
        {
            nChannel = nMax;
        }
        Buffer[nChannel] = '\0';
        mux_sprintf(newchannel->header, sizeof(newchannel->header),
                    T("%s[%s]%s"), COLOR_INTENSE, Buffer, COLOR_RESET);

        // Then, we use the non-ANSI part for the name.
        //
        nNameNoANSI = nChannel;
        pNameNoANSI = Buffer;
    }
    else
    {
        // The given channel name does contain color.
        //
        memcpy(newchannel->header, Buffer, fldChannel.m_byte + 1);
        pNameNoANSI = strip_color(Buffer, &nNameNoANSI);
    }

    // TODO: Truncation needs to be fixed.
    //
    if (nNameNoANSI > MAX_CHANNEL_LEN)
    {
        nNameNoANSI = MAX_CHANNEL_LEN;
    }

    memcpy(newchannel->name, pNameNoANSI, nNameNoANSI);
    newchannel->name[nNameNoANSI] = '\0';

    if (select_channel(newchannel->name))
    {
        raw_notify(executor, tprintf(T("Channel %s already exists."), newchannel->name));
        MEMFREE(newchannel);
        return;
    }

    newchannel->type = 127;
    newchannel->temp1 = 0;
    newchannel->temp2 = 0;
    newchannel->charge = 0;
    newchannel->charge_who = executor;
    newchannel->amount_col = 0;
    newchannel->num_users = 0;
    newchannel->max_users = 0;
    newchannel->users = nullptr;
    newchannel->on_users = nullptr;
    newchannel->chan_obj = NOTHING;
    newchannel->num_messages = 0;

    num_channels++;

    const vector<UTF8> channel_name(newchannel->name, newchannel->name + nNameNoANSI);
    mudstate.channel_names.insert(make_pair(channel_name, newchannel));

    // Report the channel creation using non-ANSI name.
    //
    raw_notify(executor, tprintf(T("Channel %s created."), newchannel->name));
}

void do_destroychannel
(
    const dbref executor,
    const dbref caller,
    const dbref enactor,
    const int eval,
    const int key,
    UTF8* channel_name,
    const UTF8* cargs[],
    const int ncargs
)
{
    UNUSED_PARAMETER(caller);
    UNUSED_PARAMETER(enactor);
    UNUSED_PARAMETER(eval);
    UNUSED_PARAMETER(key);
    UNUSED_PARAMETER(cargs);
    UNUSED_PARAMETER(ncargs);

    if (!mudconf.have_comsys)
    {
        raw_notify(executor, T("Comsys disabled."));
        return;
    }
    const auto channel_name_length = strlen(reinterpret_cast<char*>(channel_name));
    const vector<UTF8> channel_name_vector(channel_name, channel_name + channel_name_length);
    const auto it = mudstate.channel_names.find(channel_name_vector);

    if (it != mudstate.channel_names.end())
    {
        raw_notify(executor, tprintf(T("Could not find channel_name %s."), channel_name));
        return;
    }

    auto ch = it->second;

    if (  !Comm_All(executor)
       && !Controls(executor, ch->charge_who))
    {
        raw_notify(executor, NOPERM_MESSAGE);
        return;
    }
    num_channels--;

    for (int j = 0; j < ch->num_users; j++)
    {
        MEMFREE(ch->users[j]);
        ch->users[j] = nullptr;
    }
    MEMFREE(ch->users);
    ch->users = nullptr;
    MEMFREE(ch);
    ch = nullptr;
    mudstate.channel_names.erase(it);
    raw_notify(executor, tprintf(T("Channel %s destroyed."), channel_name));
}


static void do_listchannels(dbref player, UTF8* pattern)
{
    const bool perm = Comm_All(player);
    if (!perm)
    {
        raw_notify(player, T("Warning: Only public channels and your channels will be shown."));
    }

    bool bWild;
    if (nullptr != pattern
        && '\0' != *pattern)
    {
        bWild = true;
    }
    else
    {
        bWild = false;
    }

    raw_notify(player, T("*** Channel      --Flags--    Obj     Own   Charge  Balance  Users   Messages"));

    for (auto it = mudstate.channel_names.begin(); it != mudstate.channel_names.end(); ++it)
    {
        const auto ch = it->second;

        if (  perm
           || (ch->type & CHANNEL_PUBLIC)
           || Controls(player, ch->charge_who))
        {
            if (  !bWild
               || quick_wild(pattern, ch->name))
            {
                UTF8 temp[LBUF_SIZE];
                mux_sprintf(temp, sizeof(temp),
                            T("%c%c%c %-13.13s %c%c%c/%c%c%c %7d %7d %8d %8d %6d %10d"),
                            (ch->type & CHANNEL_PUBLIC) ? 'P' : '-',
                            (ch->type & CHANNEL_LOUD) ? 'L' : '-',
                            (ch->type & CHANNEL_SPOOF) ? 'S' : '-',
                            ch->name,
                            (ch->type & CHANNEL_PLAYER_JOIN) ? 'J' : '-',
                            (ch->type & CHANNEL_PLAYER_TRANSMIT) ? 'X' : '-',
                            (ch->type & CHANNEL_PLAYER_RECEIVE) ? 'R' : '-',
                            (ch->type & CHANNEL_OBJECT_JOIN) ? 'j' : '-',
                            (ch->type & CHANNEL_OBJECT_TRANSMIT) ? 'x' : '-',
                            (ch->type & CHANNEL_OBJECT_RECEIVE) ? 'r' : '-',
                            (ch->chan_obj != NOTHING) ? ch->chan_obj : -1,
                            ch->charge_who, ch->charge, ch->amount_col,
                            ch->num_users, ch->num_messages);
                raw_notify(player, temp);
            }
        }
    }
    raw_notify(player, T("-- End of list of Channels --"));
}

void do_comtitle
(
    const dbref executor,
    const dbref caller,
    const dbref enactor,
    int eval,
    const int key,
    const int nargs,
    UTF8* arg1,
    UTF8* arg2,
    const UTF8* cargs[],
    const int ncargs
)
{
    UNUSED_PARAMETER(caller);
    UNUSED_PARAMETER(enactor);
    UNUSED_PARAMETER(eval);
    UNUSED_PARAMETER(nargs);
    UNUSED_PARAMETER(cargs);
    UNUSED_PARAMETER(ncargs);

    if (!mudconf.have_comsys)
    {
        raw_notify(executor, T("Comsys disabled."));
        return;
    }
    if (!*arg1)
    {
        raw_notify(executor, T("Need an alias to do comtitle."));
        return;
    }

    UTF8 channel[MAX_CHANNEL_LEN + 1];
    mux_strncpy(channel, get_channel_from_alias(executor, arg1), MAX_CHANNEL_LEN);

    if (channel[0] == '\0')
    {
        raw_notify(executor, T("Unknown alias."));
        return;
    }
    struct channel* ch = select_channel(channel);
    if (ch)
    {
        if (select_user(ch, executor))
        {
            if (key == COMTITLE_OFF)
            {
                if ((ch->type & CHANNEL_SPOOF) == 0)
                {
                    raw_notify(executor, tprintf(T("Comtitles are now off for channel %s"), channel));
                    do_setcomtitlestatus(executor, ch, false);
                }
                else
                {
                    raw_notify(executor, T("You can not turn off comtitles on that channel."));
                }
            }
            else if (key == COMTITLE_ON)
            {
                raw_notify(executor, tprintf(T("Comtitles are now on for channel %s"), channel));
                do_setcomtitlestatus(executor, ch, true);
            }
            else
            {
                UTF8* pValidatedTitleValue = RestrictTitleValue(arg2);
                do_setnewtitle(executor, ch, pValidatedTitleValue);
                raw_notify(executor, tprintf(T("Title set to \xE2\x80\x98%s\xE2\x80\x99 on channel %s."),
                                             pValidatedTitleValue, channel));
            }
        }
    }
    else
    {
        raw_notify(executor, T("Illegal comsys alias, please delete."));
    }
}

void do_comlist
(
    const dbref executor,
    dbref caller,
    const dbref enactor,
    int eval,
    const int key,
    UTF8* pattern,
    const UTF8* cargs[],
    const int ncargs
)
{
    UNUSED_PARAMETER(caller);
    UNUSED_PARAMETER(enactor);
    UNUSED_PARAMETER(eval);
    UNUSED_PARAMETER(key);
    UNUSED_PARAMETER(cargs);
    UNUSED_PARAMETER(ncargs);

    if (!mudconf.have_comsys)
    {
        raw_notify(executor, T("Comsys disabled."));
        return;
    }

    bool bWild;
    if (nullptr != pattern
        && '\0' != *pattern)
    {
        bWild = true;
    }
    else
    {
        bWild = false;
    }

    raw_notify(executor, T("Alias           Channel            Status   Title"));

    const comsys_t* c = get_comsys(executor);
    for (int i = 0; i < c->numchannels; i++)
    {
        struct comuser* user = select_user(select_channel(c->channels[i]), executor);
        if (user)
        {
            if (!bWild
                || quick_wild(pattern, c->channels[i]))
            {
                UTF8* p =
                    tprintf(T("%-15.15s %-18.18s %s %s %s"),
                            c->alias + i * ALIAS_SIZE,
                            c->channels[i],
                            (user->bUserIsOn ? "on " : "off"),
                            (user->ComTitleStatus ? "con " : "coff"),
                            user->title);
                raw_notify(executor, p);
            }
        }
        else
        {
            raw_notify(executor, tprintf(
                           T("Bad Comsys Alias: %s for Channel: %s"), c->alias + i * ALIAS_SIZE, c->channels[i]));
        }
    }
    raw_notify(executor, T("-- End of comlist --"));
}

// Cleanup channels owned by the player.
//
void do_channelnuke(const dbref player)
{
    bool found = true;
    while (found)
    {
        found = false;
        for (auto it = mudstate.channel_names.begin(); it != mudstate.channel_names.end(); ++it)
        {
            auto ch = it->second;

            if (player == ch->charge_who)
            {
                num_channels--;

                if (nullptr != ch->users)
                {
                    for (int j = 0; j < ch->num_users; j++)
                    {
                        MEMFREE(ch->users[j]);
                        ch->users[j] = nullptr;
                    }
                    MEMFREE(ch->users);
                    ch->users = nullptr;
                }
                MEMFREE(ch);
                ch = nullptr;

                // Removing an element invalidates the iterator, so the search much be restarted.
                //
                mudstate.channel_names.erase(it);
                found = true;
                break;
            }
        }
    }
}

void do_clearcom(const dbref executor, const dbref caller, const dbref enactor, int eval, const int key)
{
    UNUSED_PARAMETER(eval);
    UNUSED_PARAMETER(key);

    if (!mudconf.have_comsys)
    {
        raw_notify(executor, T("Comsys disabled."));
        return;
    }
    const comsys_t* c = get_comsys(executor);

    for (int i = (c->numchannels) - 1; i > -1; --i)
    {
        do_delcom(executor, caller, enactor, 0, 0, c->alias + i * ALIAS_SIZE, nullptr, 0);
    }
}

void do_allcom(dbref executor, dbref caller, dbref enactor, int eval, int key, UTF8* arg1, const UTF8* cargs[],
               int ncargs)
{
    UNUSED_PARAMETER(caller);
    UNUSED_PARAMETER(enactor);
    UNUSED_PARAMETER(key);
    UNUSED_PARAMETER(eval);
    UNUSED_PARAMETER(cargs);
    UNUSED_PARAMETER(ncargs);

    if (!mudconf.have_comsys)
    {
        raw_notify(executor, T("Comsys disabled."));
        return;
    }
    if (strcmp(reinterpret_cast<char*>(arg1), "who") != 0
        && strcmp(reinterpret_cast<char*>(arg1), "on") != 0
        && strcmp(reinterpret_cast<char*>(arg1), "off") != 0)
    {
        raw_notify(executor, T("Only options available are: on, off and who."));
        return;
    }

    const comsys_t* c = get_comsys(executor);
    for (int i = 0; i < c->numchannels; i++)
    {
        do_processcom(executor, c->channels[i], arg1);
        if (strcmp(reinterpret_cast<char*>(arg1), "who") == 0)
        {
            raw_notify(executor, T(""));
        }
    }
}

void sort_users(const struct channel* ch)
{
    bool done = false;
    const int nu = ch->num_users;

    while (!done)
    {
        done = true;
        for (int i = 0; i < (nu - 1); i++)
        {
            if (ch->users[i]->who > ch->users[i + 1]->who)
            {
                struct comuser* user = ch->users[i];
                ch->users[i] = ch->users[i + 1];
                ch->users[i + 1] = user;
                done = false;
            }
        }
    }
}

void do_channelwho(const dbref executor, const dbref caller, dbref enactor, const int eval, const int key, UTF8* arg1, const UTF8* cargs[],
                   const int ncargs)
{
    UNUSED_PARAMETER(caller);
    UNUSED_PARAMETER(enactor);
    UNUSED_PARAMETER(eval);
    UNUSED_PARAMETER(key);
    UNUSED_PARAMETER(cargs);
    UNUSED_PARAMETER(ncargs);

    if (!mudconf.have_comsys)
    {
        raw_notify(executor, T("Comsys disabled."));
        return;
    }

    UTF8 channel[MAX_CHANNEL_LEN + 1];
    size_t i = 0;
    while ('\0' != arg1[i]
        && '/' != arg1[i]
        && i < MAX_CHANNEL_LEN)
    {
        channel[i] = arg1[i];
        i++;
    }
    channel[i] = '\0';

    bool bAll = false;
    if ('/' == arg1[i]
        && 'a' == arg1[i + 1])
    {
        bAll = true;
    }

    struct channel* ch = nullptr;
    if (i <= MAX_CHANNEL_LEN)
    {
        ch = select_channel(channel);
    }
    if (nullptr == ch)
    {
        raw_notify(executor, tprintf(T("Unknown channel %s."), channel));
        return;
    }
    if (!(Comm_All(executor)
        || Controls(executor, ch->charge_who)))
    {
        raw_notify(executor, NOPERM_MESSAGE);
        return;
    }

    raw_notify(executor, tprintf(T("-- %s --"), ch->name));
    raw_notify(executor, tprintf(T("%-29.29s %-6.6s %-6.6s"), "Name", "Status", "Player"));
    for (int j = 0; j < ch->num_users; j++)
    {
        const struct comuser* user = ch->users[j];
        if ((bAll
                || UNDEAD(user->who))
            && (!Hidden(user->who)
                || Wizard_Who(executor)
                || See_Hidden(executor)))
        {
            static UTF8 temp[SBUF_SIZE];
            UTF8* buff = unparse_object(executor, user->who, false);
            mux_sprintf(temp, sizeof(temp), T("%-29.29s %-6.6s %-6.6s"), strip_color(buff),
                        user->bUserIsOn ? "on " : "off",
                        isPlayer(user->who) ? "yes" : "no ");
            raw_notify(executor, temp);
            free_lbuf(buff);
        }
    }
    raw_notify(executor, tprintf(T("-- %s --"), ch->name));
}

// Assemble and transmit player disconnection messages to the player's active
// set of channels.
//
static void do_comdisconnectraw_notify(const dbref player, UTF8* chan)
{
    struct channel* ch = select_channel(chan);
    if (!ch) return;

    struct comuser* cu = select_user(ch, player);
    if (!cu) return;

    if ((ch->type & CHANNEL_LOUD)
        && cu->bUserIsOn
        && !Hidden(player))
    {
        UTF8 *messNormal, *messNoComtitle;
        BuildChannelMessage((ch->type & CHANNEL_SPOOF) != 0, ch->header, cu,
                            ch->chan_obj, (UTF8*)":has disconnected.", &messNormal,
                            &messNoComtitle);
        SendChannelMessage(player, ch, messNormal, messNoComtitle);
    }
}

// Assemble and transmit player connection messages to the player's active
// set of channels.
//
static void do_comconnectraw_notify(const dbref player, UTF8* chan)
{
    struct channel* ch = select_channel(chan);
    if (!ch) return;
    struct comuser* cu = select_user(ch, player);
    if (!cu) return;

    if ((ch->type & CHANNEL_LOUD)
        && cu->bUserIsOn
        && !Hidden(player))
    {
        UTF8 *messNormal, *messNoComtitle;
        BuildChannelMessage((ch->type & CHANNEL_SPOOF) != 0, ch->header, cu,
                            ch->chan_obj, T(":has connected."), &messNormal,
                            &messNoComtitle);
        SendChannelMessage(player, ch, messNormal, messNoComtitle);
    }
}

// Insert player into the 'on_users' list for the channel.
//
static void do_comconnectchannel(dbref player, UTF8* channel, UTF8* alias, int i)
{
    struct channel* ch = select_channel(channel);
    if (ch)
    {
        struct comuser* user;
        for (user = ch->on_users; user && user->who != player;
             user = user->on_next)
        {
        }

        if (!user)
        {
            user = select_user(ch, player);
            if (user)
            {
                user->on_next = ch->on_users;
                ch->on_users = user;
            }
            else
            {
                raw_notify(player,
                           tprintf(T("Bad Comsys Alias: %s for Channel: %s"),
                                   alias + i * ALIAS_SIZE, channel));
            }
        }
    }
    else
    {
        raw_notify(player, tprintf(T("Bad Comsys Alias: %s for Channel: %s"),
                                   alias + i * ALIAS_SIZE, channel));
    }
}

// Check player for any active channels.  If found, remove the player from
// the 'on_user' list of the channels.  Transmit disconnection messages
// as needed.
//
void do_comdisconnect(const dbref player)
{
    const comsys_t* c = get_comsys(player);

    for (int i = 0; i < c->numchannels; i++)
    {
        UTF8* CurrentChannel = c->channels[i];
        bool bFound = false;

        for (int j = 0; j < i; j++)
        {
            if (strcmp(reinterpret_cast<char*>(c->channels[j]), reinterpret_cast<char*>(CurrentChannel)) == 0)
            {
                bFound = true;
                break;
            }
        }

        if (!bFound)
        {
            // Process channel removals.
            //
            do_comdisconnectchannel(player, CurrentChannel);

            // Send disconnection messages if necessary.
            //
            do_comdisconnectraw_notify(player, CurrentChannel);
        }
    }
}

// Locate all active channels for the given player; add the player to the
// 'on_user' list of the channel and send connect notifications as
// appropriate.
//
void do_comconnect(const dbref player)
{
    const comsys_t* c = get_comsys(player);

    for (int i = 0; i < c->numchannels; i++)
    {
        UTF8* CurrentChannel = c->channels[i];
        bool bFound = false;
        int j;

        for (j = 0; j < i; j++)
        {
            if (strcmp(reinterpret_cast<char*>(c->channels[j]), reinterpret_cast<char*>(CurrentChannel)) == 0)
            {
                bFound = true;
                break;
            }
        }

        if (!bFound)
        {
            do_comconnectchannel(player, CurrentChannel, c->alias, i);
            do_comconnectraw_notify(player, CurrentChannel);
        }
    }
}


// Remove the given player from the list of active players for channel.
//
void do_comdisconnectchannel(const dbref player, UTF8* channel)
{
    struct channel* ch = select_channel(channel);
    if (!ch)
    {
        return;
    }

    struct comuser* prevuser = nullptr;
    for (struct comuser* user = ch->on_users; user;)
    {
        if (user->who == player)
        {
            if (prevuser)
            {
                prevuser->on_next = user->on_next;
            }
            else
            {
                ch->on_users = user->on_next;
            }
            return;
        }
        prevuser = user;
        user = user->on_next;
    }
}

void do_editchannel
(
    dbref executor,
    dbref caller,
    dbref enactor,
    int eval,
    int flag,
    int nargs,
    UTF8* arg1,
    UTF8* arg2,
    const UTF8* cargs[],
    int ncargs
)
{
    UNUSED_PARAMETER(caller);
    UNUSED_PARAMETER(enactor);
    UNUSED_PARAMETER(eval);
    UNUSED_PARAMETER(nargs);
    UNUSED_PARAMETER(cargs);
    UNUSED_PARAMETER(ncargs);

    if (!mudconf.have_comsys)
    {
        raw_notify(executor, T("Comsys disabled."));
        return;
    }

    struct channel* ch = select_channel(arg1);
    if (!ch)
    {
        raw_notify(executor, tprintf(T("Unknown channel %s."), arg1));
        return;
    }

    if (!(Comm_All(executor)
        || Controls(executor, ch->charge_who)))
    {
        raw_notify(executor, NOPERM_MESSAGE);
        return;
    }

    bool add_remove = true;
    UTF8* s = arg2;
    if (*s == '!')
    {
        add_remove = false;
        s++;
    }

    switch (flag)
    {
    case EDIT_CHANNEL_CCHOWN:
        {
            init_match(executor, arg2, NOTYPE);
            match_everything(0);
            const dbref who = match_result();
            if (Good_obj(who))
            {
                ch->charge_who = who;
                raw_notify(executor, T("Set."));
            }
            else
            {
                raw_notify(executor, T("Invalid player."));
            }
        }
        break;

    case EDIT_CHANNEL_CCHARGE:
        {
            const int c_charge = mux_atol(arg2);
            if (0 <= c_charge
                && c_charge <= MAX_COST)
            {
                ch->charge = c_charge;
                raw_notify(executor, T("Set."));
            }
            else
            {
                raw_notify(executor, T("That is not a reasonable cost."));
            }
        }
        break;

    case EDIT_CHANNEL_CPFLAGS:
        {
            int access = 0;
            if (strcmp(reinterpret_cast<char*>(s), "join") == 0)
            {
                access = CHANNEL_PLAYER_JOIN;
            }
            else if (strcmp(reinterpret_cast<char*>(s), "receive") == 0)
            {
                access = CHANNEL_PLAYER_RECEIVE;
            }
            else if (strcmp(reinterpret_cast<char*>(s), "transmit") == 0)
            {
                access = CHANNEL_PLAYER_TRANSMIT;
            }
            else
            {
                raw_notify(executor, T("@cpflags: Unknown Flag."));
            }

            if (access)
            {
                if (add_remove)
                {
                    ch->type |= access;
                    raw_notify(executor, T("@cpflags: Set."));
                }
                else
                {
                    ch->type &= ~access;
                    raw_notify(executor, T("@cpflags: Cleared."));
                }
            }
        }
        break;

    case EDIT_CHANNEL_COFLAGS:
        {
            int access = 0;
            if (strcmp(reinterpret_cast<char*>(s), "join") == 0)
            {
                access = CHANNEL_OBJECT_JOIN;
            }
            else if (strcmp(reinterpret_cast<char*>(s), "receive") == 0)
            {
                access = CHANNEL_OBJECT_RECEIVE;
            }
            else if (strcmp(reinterpret_cast<char*>(s), "transmit") == 0)
            {
                access = CHANNEL_OBJECT_TRANSMIT;
            }
            else
            {
                raw_notify(executor, T("@coflags: Unknown Flag."));
            }

            if (access)
            {
                if (add_remove)
                {
                    ch->type |= access;
                    raw_notify(executor, T("@coflags: Set."));
                }
                else
                {
                    ch->type &= ~access;
                    raw_notify(executor, T("@coflags: Cleared."));
                }
            }
        }
        break;
    }
}

bool test_join_access(const dbref player, struct channel* chan)
{
    if (Comm_All(player))
    {
        return true;
    }

    int access;
    if (isPlayer(player))
    {
        access = CHANNEL_PLAYER_JOIN;
    }
    else
    {
        access = CHANNEL_OBJECT_JOIN;
    }
    return ((chan->type & access) != 0
        || could_doit(player, chan->chan_obj, A_LOCK));
}

bool test_transmit_access(const dbref player, struct channel* chan)
{
    if (Comm_All(player))
    {
        return true;
    }

    int access;
    if (isPlayer(player))
    {
        access = CHANNEL_PLAYER_TRANSMIT;
    }
    else
    {
        access = CHANNEL_OBJECT_TRANSMIT;
    }
    return ((chan->type & access) != 0
        || could_doit(player, chan->chan_obj, A_LUSE));
}

bool test_receive_access(const dbref player, struct channel* chan)
{
    if (Comm_All(player))
    {
        return true;
    }

    int access;
    if (isPlayer(player))
    {
        access = CHANNEL_PLAYER_RECEIVE;
    }
    else
    {
        access = CHANNEL_OBJECT_RECEIVE;
    }
    return ((chan->type & access) != 0
        || could_doit(player, chan->chan_obj, A_LENTER));
}

// true means continue, and false means stop.
//
bool do_comsystem(const dbref who, UTF8* cmd)
{
    auto t = reinterpret_cast<UTF8*>(strchr(reinterpret_cast<char*>(cmd), ' '));
    if (!t
        || t - cmd > MAX_ALIAS_LEN
        || t[1] == '\0')
    {
        // Doesn't fit the pattern of "alias message".
        //
        return true;
    }

    UTF8 alias[ALIAS_SIZE];
    memcpy(alias, cmd, t - cmd);
    alias[t - cmd] = '\0';

    UTF8* ch = get_channel_from_alias(who, alias);
    if (ch[0] == '\0')
    {
        // Not really an alias after all.
        //
        return true;
    }

    t++;
    do_processcom(who, ch, t);
    return false;
}

void do_cemit
(
    const dbref executor,
    const dbref caller,
    const dbref enactor,
    const int eval,
    const int key,
    const int nargs,
    UTF8* chan,
    UTF8* text,
    const UTF8* cargs[],
    const int ncargs
)
{
    UNUSED_PARAMETER(caller);
    UNUSED_PARAMETER(enactor);
    UNUSED_PARAMETER(eval);
    UNUSED_PARAMETER(nargs);
    UNUSED_PARAMETER(cargs);
    UNUSED_PARAMETER(ncargs);

    if (!mudconf.have_comsys)
    {
        raw_notify(executor, T("Comsys disabled."));
        return;
    }
    struct channel* ch = select_channel(chan);
    if (!ch)
    {
        raw_notify(executor, tprintf(T("Channel %s does not exist."), chan));
        return;
    }
    if (!Controls(executor, ch->charge_who)
        && !Comm_All(executor))
    {
        raw_notify(executor, NOPERM_MESSAGE);
        return;
    }
    UTF8* text2 = alloc_lbuf("do_cemit");
    if (key == CEMIT_NOHEADER)
    {
        mux_strncpy(text2, text, LBUF_SIZE - 1);
    }
    else
    {
        mux_strncpy(text2, tprintf(T("%s %s"), ch->header, text), LBUF_SIZE - 1);
    }
    SendChannelMessage(executor, ch, text2, text2);
}

void do_chopen
(
    const dbref executor,
    const dbref caller,
    const dbref enactor,
    const int eval,
    const int key,
    const int nargs,
    UTF8* chan,
    UTF8* value,
    const UTF8* cargs[],
    int ncargs
)
{
    UNUSED_PARAMETER(eval);
    UNUSED_PARAMETER(nargs);
    UNUSED_PARAMETER(cargs);
    UNUSED_PARAMETER(ncargs);

    if (!mudconf.have_comsys)
    {
        raw_notify(executor, T("Comsys disabled."));
        return;
    }
    if (key == CSET_LIST)
    {
        do_chanlist(executor, caller, enactor, 0, 1, nullptr, nullptr, 0);
        return;
    }

    const UTF8* msg = nullptr;
    struct channel* ch = select_channel(chan);
    if (!ch)
    {
        msg = tprintf(T("@cset: Channel %s does not exist."), chan);
        raw_notify(executor, msg);
        return;
    }
    if (!Controls(executor, ch->charge_who)
        && !Comm_All(executor))
    {
        raw_notify(executor, NOPERM_MESSAGE);
        return;
    }
    dbref thing;

    switch (key)
    {
    case CSET_PUBLIC:
        ch->type |= CHANNEL_PUBLIC;
        msg = tprintf(T("@cset: Channel %s placed on the public listings."), chan);
        break;

    case CSET_PRIVATE:
        ch->type &= ~CHANNEL_PUBLIC;
        msg = tprintf(T("@cset: Channel %s taken off the public listings."), chan);
        break;

    case CSET_LOUD:
        ch->type |= CHANNEL_LOUD;
        msg = tprintf(T("@cset: Channel %s now sends connect/disconnect msgs."), chan);
        break;

    case CSET_QUIET:
        ch->type &= ~CHANNEL_LOUD;
        msg = tprintf(T("@cset: Channel %s connect/disconnect msgs muted."), chan);
        break;

    case CSET_SPOOF:
        ch->type |= CHANNEL_SPOOF;
        msg = tprintf(T("@cset: Channel %s set spoofable."), chan);
        break;

    case CSET_NOSPOOF:
        ch->type &= ~CHANNEL_SPOOF;
        msg = tprintf(T("@cset: Channel %s set unspoofable."), chan);
        break;

    case CSET_OBJECT:
        init_match(executor, value, NOTYPE);
        match_everything(0);
        thing = match_result();

        if (thing == NOTHING)
        {
            ch->chan_obj = thing;
            msg = tprintf(T("Channel %s is now disassociated from any channel object."), ch->name);
        }
        else if (Good_obj(thing))
        {
            ch->chan_obj = thing;
            UTF8* buff = unparse_object(executor, thing, false);
            msg = tprintf(T("Channel %s is now using %s as channel object."), ch->name, buff);
            free_lbuf(buff);
        }
        else
        {
            msg = tprintf(T("%d is not a valid channel object."), thing);
        }
        break;

    case CSET_HEADER:
        do_cheader(executor, chan, value);
        msg = T("Set.");
        break;

    case CSET_LOG:
        if (do_chanlog(executor, chan, value))
        {
            msg = tprintf(T("@cset: Channel %s maximum history set."), chan);
        }
        else
        {
            msg = tprintf(T("@cset: Maximum history must be a number less than or equal to %d."), MAX_RECALL_REQUEST);
        }
        break;

    case CSET_LOG_TIME:
        if (do_chanlog_timestamps(executor, chan, value))
        {
            msg = tprintf(T("@cset: Channel %s timestamp logging set."), chan);
        }
        else
        {
            msg = tprintf(T("@cset: Failed.  Is logging enabled for %s?"), chan);
        }
    }
    raw_notify(executor, msg);
}

void do_chboot
(
    const dbref executor,
    const dbref caller,
    const dbref enactor,
    const int eval,
    int key,
    const int nargs,
    UTF8* channel,
    UTF8* victim,
    const UTF8* cargs[],
    int ncargs
)
{
    UNUSED_PARAMETER(caller);
    UNUSED_PARAMETER(enactor);
    UNUSED_PARAMETER(eval);
    UNUSED_PARAMETER(nargs);
    UNUSED_PARAMETER(cargs);
    UNUSED_PARAMETER(ncargs);

    // I sure hope it's not going to be that long.
    //
    if (!mudconf.have_comsys)
    {
        raw_notify(executor, T("Comsys disabled."));
        return;
    }
    struct channel* ch = select_channel(channel);
    if (!ch)
    {
        raw_notify(executor, T("@cboot: Unknown channel."));
        return;
    }
    struct comuser* user = select_user(ch, executor);
    if (!user)
    {
        raw_notify(executor, T("@cboot: You are not on that channel."));
        return;
    }
    if (!Controls(executor, ch->charge_who)
        && !Comm_All(executor))
    {
        raw_notify(executor, T("@cboot: You can\xE2\x80\x99t do that!"));
        return;
    }
    const dbref thing = match_thing(executor, victim);

    if (!Good_obj(thing))
    {
        return;
    }
    struct comuser* vu = select_user(ch, thing);
    if (!vu)
    {
        raw_notify(executor, tprintf(T("@cboot: %s is not on the channel."),
                                     Moniker(thing)));
        return;
    }

    raw_notify(executor, tprintf(T("You boot %s off channel %s."),
                                 Moniker(thing), ch->name));
    raw_notify(thing, tprintf(T("%s boots you off channel %s."),
                              Moniker(thing), ch->name));

    if (!(key & CBOOT_QUIET))
    {
        UTF8 *mess1, *mess1nct;
        UTF8 *mess2, *mess2nct;
        BuildChannelMessage((ch->type & CHANNEL_SPOOF) != 0, ch->header, user,
                            ch->chan_obj, T(":boots"), &mess1, &mess1nct);
        BuildChannelMessage((ch->type & CHANNEL_SPOOF) != 0, nullptr, vu,
                            ch->chan_obj, T(":off the channel."), &mess2,
                            &mess2nct);
        UTF8* messNormal = alloc_lbuf("do_chboot.messnormal");
        UTF8* messNoComtitle = alloc_lbuf("do_chboot.messnocomtitle");
        UTF8* mnp = messNormal;
        UTF8* mnctp = messNoComtitle;
        if (mess1)
        {
            safe_str(mess1, messNormal, &mnp);
            free_lbuf(mess1);
        }
        if (mess2)
        {
            safe_str(mess2, messNormal, &mnp);
            free_lbuf(mess2);
        }
        *mnp = '\0';
        if (mess1nct)
        {
            safe_str(mess1nct, messNoComtitle, &mnctp);
            free_lbuf(mess1nct);
        }
        if (mess2nct)
        {
            safe_str(mess2nct, messNoComtitle, &mnctp);
            free_lbuf(mess2nct);
        }
        *mnctp = '\0';
        SendChannelMessage(executor, ch, messNormal, messNoComtitle);
        do_delcomchannel(thing, channel, false);
    }
    else
    {
        do_delcomchannel(thing, channel, true);
    }
}

// Process a channel header set request.
//
void do_cheader(const dbref player, UTF8* channel, const UTF8* header)
{
    struct channel* ch = select_channel(channel);
    if (!ch)
    {
        raw_notify(player, T("That channel does not exist."));
        return;
    }
    if (!Controls(player, ch->charge_who)
        && !Comm_All(player))
    {
        raw_notify(player, NOPERM_MESSAGE);
        return;
    }

    // Optimize/terminate any ANSI in the string.
    //
    UTF8 NewHeader_ANSI[MAX_HEADER_LEN + 1];
    const mux_field nLen = StripTabsAndTruncate(header, NewHeader_ANSI,
                                                MAX_HEADER_LEN, MAX_HEADER_LEN);
    memcpy(ch->header, NewHeader_ANSI, nLen.m_byte + 1);
}

struct chanlist_node
{
    UTF8* name;
    struct channel* ptr;
};

static int DCL_CDECL chanlist_comp(const void* a, const void* b)
{
    const auto ca = (chanlist_node*)a;
    const auto cb = (chanlist_node*)b;
    return mux_stricmp(ca->name, cb->name);
}

void do_chanlist
(
    const dbref executor,
    const dbref caller,
    const dbref enactor,
    const int eval,
    int key,
    UTF8* pattern,
    const UTF8* cargs[],
    const int ncargs
)
{
    UNUSED_PARAMETER(caller);
    UNUSED_PARAMETER(enactor);
    UNUSED_PARAMETER(eval);
    UNUSED_PARAMETER(cargs);
    UNUSED_PARAMETER(ncargs);

    if (!mudconf.have_comsys)
    {
        raw_notify(executor, T("Comsys disabled."));
        return;
    }
    if (key & CLIST_FULL)
    {
        do_listchannels(executor, pattern);
        return;
    }

    dbref owner;
    int flags = 0;

    if (key & CLIST_HEADERS)
    {
        raw_notify(executor, T("*** Channel       Owner           Header"));
    }
    else
    {
        raw_notify(executor, T("*** Channel       Owner           Description"));
    }

    bool bWild;
    if (nullptr != pattern
        && '\0' != *pattern)
    {
        bWild = true;
    }
    else
    {
        bWild = false;
    }

#define MAX_SUPPORTED_NUM_ENTRIES 10000

    auto entries = mudstate.channel_names.size();
    if (MAX_SUPPORTED_NUM_ENTRIES < entries)
    {
        // Nobody should have so many channels.
        //
        entries = MAX_SUPPORTED_NUM_ENTRIES;
    }

    if (0 < entries)
    {
        for (auto it = mudstate.channel_names.begin(); it != mudstate.channel_names.end(); ++it)
        {
            const auto ch = it->second;

            if (!bWild != quick_wild(pattern, ch->name))
            {
                if (Comm_All(executor)
                    || (ch->type & CHANNEL_PUBLIC)
                    || Controls(executor, ch->charge_who))
                {
                    const UTF8* pBuffer = nullptr;
                    UTF8* atrstr = nullptr;

                    if (key & CLIST_HEADERS)
                    {
                        pBuffer = ch->header;
                    }
                    else
                    {
                        if (NOTHING != ch->chan_obj)
                        {
                            atrstr = atr_pget(ch->chan_obj, A_DESC, &owner, &flags);
                        }

                        if (nullptr != atrstr && '\0' != atrstr[0])
                        {
                            pBuffer = atrstr;
                        }
                        else
                        {
                            pBuffer = T("No description.");
                        }
                    }

                    UTF8* temp = alloc_mbuf("do_chanlist_temp");
                    mux_sprintf(temp, MBUF_SIZE, T("%c%c%c "),
                        (ch->type & (CHANNEL_PUBLIC)) ? 'P' : '-',
                        (ch->type & (CHANNEL_LOUD)) ? 'L' : '-',
                        (ch->type & (CHANNEL_SPOOF)) ? 'S' : '-');
                    mux_field iPos(4, 4);

                    iPos += StripTabsAndTruncate(ch->name,
                        temp + iPos.m_byte,
                        (MBUF_SIZE - 1) - iPos.m_byte,
                        13);
                    iPos = PadField(temp, MBUF_SIZE - 1, 18, iPos);
                    iPos += StripTabsAndTruncate(Moniker(ch->charge_who),
                        temp + iPos.m_byte,
                        (MBUF_SIZE - 1) - iPos.m_byte,
                        15);
                    iPos = PadField(temp, MBUF_SIZE - 1, 34, iPos);
                    iPos += StripTabsAndTruncate(pBuffer,
                        temp + iPos.m_byte,
                        (MBUF_SIZE - 1) - iPos.m_byte,
                        45);
                    iPos = PadField(temp, MBUF_SIZE - 1, 79, iPos);

                    raw_notify(executor, temp);
                    free_mbuf(temp);

                    if (nullptr != atrstr)
                    {
                        free_lbuf(atrstr);
                    }
                }
            }
        }
    }
    raw_notify(executor, T("-- End of list of Channels --"));
}

// Returns a player's comtitle for a named channel.
//
FUNCTION(fun_comtitle)
{
    UNUSED_PARAMETER(caller);
    UNUSED_PARAMETER(enactor);
    UNUSED_PARAMETER(eval);
    UNUSED_PARAMETER(nfargs);
    UNUSED_PARAMETER(cargs);
    UNUSED_PARAMETER(ncargs);

    if (!mudconf.have_comsys)
    {
        safe_str(T("#-1 COMSYS DISABLED"), buff, bufc);
        return;
    }

    dbref victim = lookup_player(executor, fargs[0], true);
    if (!Good_obj(victim))
    {
        init_match(executor, fargs[0], TYPE_THING);
        match_everything(0);
        victim = match_result();
        if (!Good_obj(victim))
        {
            safe_str(T("#-1 OBJECT DOES NOT EXIST"), buff, bufc);
            return;
        }
    }

    struct channel* chn = select_channel(fargs[1]);
    if (!chn)
    {
        safe_str(T("#-1 CHANNEL DOES NOT EXIST"), buff, bufc);
        return;
    }

    comsys_t* c = get_comsys(executor);
    struct comuser* user;

    int i;
    bool onchannel = false;
    if (Wizard(executor))
    {
        onchannel = true;
    }
    else
    {
        for (i = 0; i < c->numchannels; i++)
        {
            user = select_user(chn, executor);
            if (user)
            {
                onchannel = true;
                break;
            }
        }
    }

    if (!onchannel)
    {
        safe_noperm(buff, bufc);
        return;
    }

    for (i = 0; i < c->numchannels; i++)
    {
        user = select_user(chn, victim);
        if (user)
        {
            // Do we want this function to evaluate the comtitle or not?
#if 0
          UTF8 *nComTitle = GetComtitle(user);
          safe_str(nComTitle, buff, bufc);
          FreeComtitle(nComTitle);
          return;
#else
            safe_str(user->title, buff, bufc);
            return;
#endif
        }
    }
    safe_str(T("#-1 OBJECT NOT ON THAT CHANNEL"), buff, bufc);
}

// Returns a player's comsys alias for a named channel.
//
FUNCTION(fun_comalias)
{
    UNUSED_PARAMETER(caller);
    UNUSED_PARAMETER(enactor);
    UNUSED_PARAMETER(eval);
    UNUSED_PARAMETER(nfargs);
    UNUSED_PARAMETER(cargs);
    UNUSED_PARAMETER(ncargs);

    if (!mudconf.have_comsys)
    {
        safe_str(T("#-1 COMSYS DISABLED"), buff, bufc);
        return;
    }

    dbref victim = lookup_player(executor, fargs[0], true);
    if (!Good_obj(victim))
    {
        init_match(executor, fargs[0], TYPE_THING);
        match_everything(0);
        victim = match_result();
        if (!Good_obj(victim))
        {
            safe_str(T("#-1 OBJECT DOES NOT EXIST"), buff, bufc);
            return;
        }
    }

    struct channel* chn = select_channel(fargs[1]);
    if (!chn)
    {
        safe_str(T("#-1 CHANNEL DOES NOT EXIST"), buff, bufc);
        return;
    }

    // Wizards can get the comalias for anyone. Players and objects can check
    // for themselves. Objects that Inherit can check for their owners.
    //
    if (!Wizard(executor)
        && executor != victim
        && (Owner(executor) != victim
            || !Inherits(executor)))
    {
        safe_noperm(buff, bufc);
        return;
    }

    const comsys_t* cc = get_comsys(victim);
    for (int i = 0; i < cc->numchannels; i++)
    {
        if (!strcmp(reinterpret_cast<char*>(fargs[1]), reinterpret_cast<char*>(cc->channels[i])))
        {
            safe_str(cc->alias + i * ALIAS_SIZE, buff, bufc);
            return;
        }
    }
    safe_str(T("#-1 OBJECT NOT ON THAT CHANNEL"), buff, bufc);
}

// Returns a list of channels.
//
FUNCTION(fun_channels)
{
    UNUSED_PARAMETER(caller);
    UNUSED_PARAMETER(enactor);
    UNUSED_PARAMETER(eval);
    UNUSED_PARAMETER(cargs);
    UNUSED_PARAMETER(ncargs);

    if (!mudconf.have_comsys)
    {
        safe_str(T("#-1 COMSYS DISABLED"), buff, bufc);
        return;
    }

    dbref who = NOTHING;
    if (nfargs >= 1)
    {
        who = lookup_player(executor, fargs[0], true);
        if (who == NOTHING
            && mux_stricmp(fargs[0], T("all")) != 0)
        {
            safe_str(T("#-1 PLAYER NOT FOUND"), buff, bufc);
            return;
        }
    }

    ITL itl;
    ItemToList_Init(&itl, buff, bufc);
    for (auto it = mudstate.channel_names.begin(); it != mudstate.channel_names.end(); ++it)
    {
        const auto ch = it->second;

        if ((Comm_All(executor)
                || (ch->type & CHANNEL_PUBLIC)
                || Controls(executor, ch->charge_who))
            && (who == NOTHING
                || Controls(who, ch->charge_who))
            && !ItemToList_AddString(&itl, ch->name))
        {
            break;
        }
    }
}

FUNCTION(fun_chanobj)
{
    UNUSED_PARAMETER(executor);
    UNUSED_PARAMETER(caller);
    UNUSED_PARAMETER(enactor);
    UNUSED_PARAMETER(eval);
    UNUSED_PARAMETER(nfargs);
    UNUSED_PARAMETER(cargs);
    UNUSED_PARAMETER(ncargs);

    if (!mudconf.have_comsys)
    {
        safe_str(T("#-1 COMSYS DISABLED"), buff, bufc);
        return;
    }

    struct channel* ch = select_channel(fargs[0]);
    if (nullptr == ch)
    {
        safe_str(T("#-1 CHANNEL NOT FOUND"), buff, bufc);
        return;
    }

    const dbref obj = ch->chan_obj;
    if (Good_obj(obj))
    {
        safe_str(tprintf(T("#%d"), obj), buff, bufc);
    }
    else
    {
        safe_str(T("#-1"), buff, bufc);
    }
}
