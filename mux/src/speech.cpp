/*! \file speech.cpp
 * \brief Commands which involve speaking.
 *
 */

#include "copyright.h"
#include "autoconf.h"
#include "config.h"
#include "externs.h"

UTF8* modSpeech(dbref player, const UTF8* message, bool bWhich, const UTF8* command)
{
    dbref aowner;
    int aflags;
    UTF8* mod = atr_get("modSpeech.25", player, bWhich ? A_SPEECHMOD : A_SAYSTRING,
        &aowner, &aflags);

    if (mod[0] == '\0'
        || alarm_clock.alarmed)
    {
        free_lbuf(mod);
        return nullptr;
    }

    UTF8* new_message = alloc_lbuf("modspeech");
    UTF8* t_ptr = new_message;
    const UTF8* args[2];
    args[0] = message;
    args[1] = command;
    mux_exec(mod, LBUF_SIZE - 1, new_message, &t_ptr, player, player, player,
        AttrTrace(aflags, EV_FCHECK | EV_EVAL | EV_TOP), args, 2);
    *t_ptr = '\0';
    free_lbuf(mod);
    return new_message;
}

static int idle_timeout_val(dbref player)
{
    // If IDLETIMEOUT attribute is not present, the value
    // returned will be zero.
    //
    dbref aowner;
    int aflags;
    UTF8* ITbuffer = atr_get("idle_timeout_val.53", player, A_IDLETMOUT, &aowner, &aflags);
    int idle_timeout = mux_atol(ITbuffer);
    free_lbuf(ITbuffer);
    return idle_timeout;
}

static bool sp_ok(dbref player)
{
    if (Gagged(player)
        && !Wizard(player))
    {
        notify(player, T("Sorry. Gagged players cannot speak."));
        return false;
    }

    if (!mudconf.robot_speak)
    {
        if (Robot(player) && !Controls(player, Location(player)))
        {
            notify(player, T("Sorry, robots may not speak in public."));
            return false;
        }
    }
    if (Auditorium(Location(player)))
    {
        if (!could_doit(player, Location(player), A_LSPEECH))
        {
            notify(player, T("Sorry, you may not speak in this place."));
            return false;
        }
    }
    return true;
}

void do_think(dbref executor, dbref caller, dbref enactor, int eval, int key,
    UTF8* message, const UTF8* cargs[], int ncargs)
{
    UNUSED_PARAMETER(key);
    UNUSED_PARAMETER(cargs);
    UNUSED_PARAMETER(ncargs);

    UTF8* buf, * bp;

    buf = bp = alloc_lbuf("do_think");
    mux_exec(message, LBUF_SIZE - 1, buf, &bp, executor, caller, enactor, eval | EV_FCHECK | EV_EVAL | EV_TOP,
        nullptr, 0);
    *bp = '\0';
    notify(executor, buf);
    free_lbuf(buf);
}

void do_say(dbref executor, dbref caller, dbref enactor, int eval, int key, UTF8* message, const UTF8* cargs[], int ncargs)
{
    UNUSED_PARAMETER(caller);
    UNUSED_PARAMETER(enactor);
    UNUSED_PARAMETER(eval);
    UNUSED_PARAMETER(cargs);
    UNUSED_PARAMETER(ncargs);

    // Make sure speaker is somewhere if speaking in a place
    //
    dbref loc = where_is(executor);
    if (!(Good_obj(loc)
        && sp_ok(executor)))
    {
        return;
    }

    int say_flags, depth;

    // Convert prefix-coded messages into the normal type
    //
    say_flags = key & (SAY_NOEVAL | SAY_HERE | SAY_ROOM | SAY_HTML);
    key &= ~(SAY_NOEVAL | SAY_HERE | SAY_ROOM | SAY_HTML);

    char noSpaceChars[] = { '\'', '#', ':' };
    bool noSpaceCharFound = false;

    if (key == SAY_PREFIX)
    {
        switch (message[0])
        {
        case '"':
            message++;
            key = SAY_SAY;
            break;

        case ':':
            message++;
            while (message[0] == ' ')
            {
                message++;
            }
            for (int i = 0; i < sizeof noSpaceChars; i++)
            {
                if (message[0] == noSpaceChars[i])
                {
                    noSpaceCharFound = true;
                    break;
                }
            }
            if (noSpaceCharFound)
            {
                key = SAY_POSE_NOSPC;
            }
            else
            {
                key = SAY_POSE;
            }
            break;

        case ';':
            message++;
            key = SAY_POSE_NOSPC;
            break;

        case 0xE2:
            if (0x80 == message[1]
                && 0x9C == message[2])
            {
                // U+201C - Unicode version of opening double quote.
                //
                message += 3;
                key = SAY_SAY;
            }
            else
            {
                key = SAY_EMIT;
            }
            break;

        case '\\':
            message++;

            // FALLTHROUGH
            //

        default:
            key = SAY_EMIT;
            break;
        }
    }

    UTF8* command = (UTF8*)"";
    if (SAY_SAY == key)
    {
        command = (UTF8*)"say";
    }
    else if (SAY_POSE == key || SAY_POSE_NOSPC == key)
    {
        command = (UTF8*)"pose";
    }
    else if (SAY_EMIT == key)
    {
        command = (UTF8*)"@emit";
    }

    // Parse speechmod if present.
    //
    UTF8* messageOrig = message;
    UTF8* messageNew = nullptr;
    if (!(say_flags & SAY_NOEVAL))
    {
        messageNew = modSpeech(executor, message, true, command);
        if (messageNew)
        {
            message = messageNew;
        }
    }

    // Send the message on its way
    //
    UTF8* saystring;
    switch (key)
    {
    case SAY_SAY:
        saystring = modSpeech(executor, messageOrig, false, command);
        if (saystring)
        {
#ifdef REALITY_LVLS
            notify_except_rlevel(loc, executor, -1, tprintf(T("%s %s \"%s\""), Moniker(executor), saystring, message), MSG_SAYPOSE);
#else
            notify_all_from_inside_saypose(loc, executor, tprintf(T("%s %s \"%s\""), Moniker(executor), saystring, message));
#endif
            free_lbuf(saystring);
        }
        else
        {
#ifdef REALITY_LVLS
            notify_except_rlevel(loc, executor, executor, tprintf(T("%s says, \"%s\""), Moniker(executor), message), MSG_SAYPOSE);
#else
            notify_all_from_inside_saypose(loc, executor, tprintf(T("%s says, \"%s\""), Moniker(executor), message));
#endif
        }
        break;

    case SAY_POSE:
#ifdef REALITY_LVLS
        notify_except_rlevel(loc, executor, -1, tprintf(T("%s %s"), Moniker(executor), message), MSG_SAYPOSE);
#else
        notify_all_from_inside_saypose(loc, executor, tprintf(T("%s %s"), Moniker(executor), message));
#endif
        break;

    case SAY_POSE_NOSPC:
#ifdef REALITY_LVLS
        notify_except_rlevel(loc, executor, -1, tprintf(T("%s%s"), Moniker(executor), message), MSG_SAYPOSE);
#else
        notify_all_from_inside_saypose(loc, executor, tprintf(T("%s%s"), Moniker(executor), message));
#endif
        break;

    case SAY_EMIT:
        if ((say_flags & SAY_HERE)
            || (say_flags & SAY_HTML)
            || !say_flags)
        {
            if (say_flags & SAY_HTML)
            {
                notify_all_from_inside_html(loc, executor, message);
            }
            else
            {

#ifdef REALITY_LVLS
                notify_except_rlevel(loc, executor, -1, message, SAY_EMIT);
#else
                notify_all_from_inside(loc, executor, message);
#endif
            }
        }
        if (say_flags & SAY_ROOM)
        {
            if (isRoom(loc)
                && (say_flags & SAY_HERE))
            {
                if (messageNew)
                {
                    free_lbuf(messageNew);
                }
                return;
            }
            for (depth = 0; !isRoom(loc) && (depth < 20); depth++)
            {
                loc = Location(loc);
                if (!Good_obj(loc)
                    || (loc == Location(loc)))
                {
                    if (messageNew)
                    {
                        free_lbuf(messageNew);
                    }
                    return;
                }
            }
            if (isRoom(loc))
            {
#ifdef REALITY_LVLS
                notify_except_rlevel(loc, executor, -1, message, -1);
#else
                notify_all_from_inside(loc, executor, message);
#endif
            }
        }
        break;
    }
    if (messageNew)
    {
        free_lbuf(messageNew);
    }
}

static void wall_broadcast(int target, dbref player, UTF8* message)
{
    for (auto it = mudstate.descriptor_list.begin(); it != mudstate.descriptor_list.end(); ++it)
    {
        DESC* d = *it;
        if (d->flags & DS_CONNECTED)
        {
            switch (target)
            {
            case SHOUT_WIZARD:

                if (Wizard(d->player))
                {
                    notify_with_cause(d->player, player, message);
                }
                break;

            case SHOUT_ADMIN:

                if (WizRoy(d->player))
                {
                    notify_with_cause(d->player, player, message);
                }
                break;

            default:

                notify_with_cause(d->player, player, message);
                break;
            }
        }
    }
}

static const UTF8* announce_msg = T("Announcement: ");
static const UTF8* broadcast_msg = T("Broadcast: ");
static const UTF8* admin_msg = T("Admin: ");

void do_shout(dbref executor, dbref caller, dbref enactor, int eval, int key,
    UTF8* message, const UTF8* cargs[], int ncargs)
{
    UNUSED_PARAMETER(caller);
    UNUSED_PARAMETER(enactor);
    UNUSED_PARAMETER(eval);
    UNUSED_PARAMETER(cargs);
    UNUSED_PARAMETER(ncargs);

    UTF8* p = nullptr, * messageNew = nullptr, * buf2 = nullptr, * bp = nullptr;
    bool bNoTag = (key & SHOUT_NOTAG) ? true : false;
    bool bEmit = (key & SHOUT_EMIT) ? true : false;
    bool bPose = (key & SHOUT_POSE) ? true : bEmit;
    bool bSpace = !bEmit;
    key &= ~(SHOUT_NOTAG | SHOUT_POSE | SHOUT_EMIT);
    static const UTF8* prefix, * loghead, * logtext1, * logsay, * saystring;

    if (key & SHOUT_ADMIN)
    {
        key = SHOUT_ADMIN;      // @wall/wiz/admin is treated as @wall/admin
        prefix = admin_msg;
        loghead = T("ASHOUT");
        logtext1 = T(" ADMIN");
        logsay = T(" yells: ");
        saystring = T("says, \"");
    }
    else if (key & SHOUT_WIZARD)
    {
        prefix = broadcast_msg;
        loghead = T("BCAST");
        logtext1 = T(" WIZ");
        logsay = T(" broadcasts: ");
        saystring = T("says, \"");
    }
    else
    {
        prefix = announce_msg;
        loghead = T("SHOUT");
        logtext1 = T(" WALL");
        logsay = T(" shouts: ");
        saystring = T("shouts, \"");
    }

    if (bNoTag)
    {
        prefix = T("");
    }

    if (!bPose)
    {
        switch (*message)
        {
        case ';':
            bSpace = false;
            // FALL THROUGH

        case ':':
            bPose = true;
            // FALL THROUGH

        case '"':
            message++;
            break;
        }
    }
    // Parse speechmod if present.
    //
    messageNew = modSpeech(executor, message, true, (UTF8*)"@wall");
    if (messageNew)
    {
        message = messageNew;
    }
    if (!bPose)
    {
        buf2 = alloc_lbuf("do_shout");
        bp = buf2;
        safe_str(saystring, buf2, &bp);
        safe_str(message, buf2, &bp);
        safe_str(T("\""), buf2, &bp);
        *bp = '\0';
    }
    p = tprintf(T("%s%s%s%s"), prefix, bEmit ? T("") : Moniker(executor),
        bSpace ? T(" ") : T(""), bPose ? (UTF8*)message : (UTF8*)buf2);
    wall_broadcast(key, executor, p);
    if (!bPose)
    {
        free_lbuf(buf2);
    }
    STARTLOG(LOG_SHOUTS, "WIZ", loghead);
    log_name(executor);
    if (bEmit)
    {
        log_text(logtext1);
        log_text(T("emits: "));
    }
    else if (bPose)
    {
        log_text(logtext1);
        log_text(T("poses: "));
    }
    else
    {
        log_text(logsay);
    }
    log_text(message);
    ENDLOG;

    if (messageNew)
    {
        free_lbuf(messageNew);
    }
}

/* ---------------------------------------------------------------------------
 * do_page: Handle the page command.
 * Page-pose code from shadow@prelude.cc.purdue.
 */

static void page_return(dbref player, dbref target, const UTF8* tag,
    int anum, const UTF8* dflt)
{
    if (alarm_clock.alarmed)
    {
        return;
    }

    dbref aowner;
    int aflags;
    UTF8* str, * str2, * bp;

    str = atr_pget(target, anum, &aowner, &aflags);
    if (*str)
    {
        str2 = bp = alloc_lbuf("page_return");
        mux_exec(str, LBUF_SIZE - 1, str2, &bp, target, player, player,
            AttrTrace(aflags, EV_FCHECK | EV_EVAL | EV_TOP | EV_NO_LOCATION),
            nullptr, 0);
        *bp = '\0';
        if (*str2)
        {
            CLinearTimeAbsolute ltaNow;
            ltaNow.GetLocal();
            FIELDEDTIME ft;
            ltaNow.ReturnFields(&ft);

            UTF8* p = tprintf(T("%s message from %s: %s"), tag,
                Moniker(target), str2);
            notify_with_cause_ooc(player, target, p, MSG_SRC_PAGE);
            p = tprintf(T("[%d:%02d] %s message sent to %s."), ft.iHour,
                ft.iMinute, tag, Moniker(player));
            notify_with_cause_ooc(target, player, p, MSG_SRC_PAGE);
        }
        free_lbuf(str2);
    }
    else if (dflt && *dflt)
    {
        notify_with_cause_ooc(player, target, dflt, MSG_SRC_PAGE);
    }
    free_lbuf(str);
}

static bool page_check(dbref player, dbref target)
{
    if (!payfor(player, Guest(player) ? 0 : mudconf.pagecost))
    {
        notify(player, tprintf(T("You don\'t have enough %s."), mudconf.many_coins));
    }
    else if (!Connected(target) && !Connected(Owner(target)))
    {
        page_return(player, target, T("Away"), A_AWAY,
            tprintf(T("Sorry, %s is not connected."), Moniker(target)));
    }
    else if (!could_doit(player, target, A_LPAGE))
    {
        if (Can_Hide(target)
            && Hidden(target)
            && !See_Hidden(player))
        {
            page_return(player, target, T("Away"), A_AWAY,
                tprintf(T("Sorry, %s is not connected."), Moniker(target)));
        }
        else
        {
            page_return(player, target, T("Reject"), A_REJECT,
                tprintf(T("Sorry, %s is not accepting pages."), Moniker(target)));
        }
    }
    else if (!could_doit(target, player, A_LPAGE))
    {
        if (Wizard(player))
        {
            notify(player, tprintf(T("Warning: %s can\'t return your page."),
                Moniker(target)));
            return true;
        }
        else
        {
            notify(player, tprintf(T("Sorry, %s can\'t return your page."),
                Moniker(target)));
            return false;
        }
    }
    else
    {
        return true;
    }
    return false;
}

// The combinations are:
//
//           nargs  arg1[0]  arg2[0]
//   ''        1      '\0'    '\0'      Report LastPaged to player.
//   'a'       1      'a'     '\0'      Page LastPaged with A
//   'a='      2      'a'     '\0'      Page A. LastPaged <- A
//   '=b'      2      '\0'    'b'       Page LastPaged with B
//   'a=b'     2      'a'     'b'       Page A with B. LastPaged <- A
//   'a=b1=[b2=]*...'                   All treated the same as 'a=b'.
//
void do_page
(
    dbref executor,
    dbref caller,
    dbref enactor,
    int   eval,
    int   key,
    int   nargs,
    UTF8* arg1,
    UTF8* arg2,
    const UTF8* cargs[],
    int   ncargs
)
{
    UNUSED_PARAMETER(caller);
    UNUSED_PARAMETER(enactor);
    UNUSED_PARAMETER(eval);
    UNUSED_PARAMETER(key);
    UNUSED_PARAMETER(cargs);
    UNUSED_PARAMETER(ncargs);

    int   nPlayers = 0;
    dbref aPlayers[(LBUF_SIZE + 1) / 2];

    // Either we have been given a recipient list, or we are relying on an
    // existing A_LASTPAGE.
    //
    bool bModified = false;
    if (nargs == 2
        && arg1[0] != '\0')
    {
        bModified = true;

        UTF8* p = arg1;
        while (*p != '\0')
        {
            UTF8* q = (UTF8*)strchr((char*)p, '"');
            if (q)
            {
                *q = '\0';
            }

            // Decode space-delimited or comma-delimited recipients.
            //
            string_token st(p, T(","));
            UTF8* r;
            for (r = st.parse(); r; r = st.parse())
            {
                dbref target = lookup_player(executor, r, true);

                if (target == NOTHING)
                {
                    init_match(executor, r, NOTYPE);
                    match_neighbor();
                    target = match_result();
                }

                if (target >= 0)
                {
                    aPlayers[nPlayers++] = target;
                }
                else if (target == AMBIGUOUS)
                {
                    notify(executor, tprintf(T("I found more than one \"%s\"."), trim_spaces((const UTF8 *)r)));
                }
                else if (target)
                {
                    notify(executor, tprintf(T("I don\'t recognize \"%s\"."), trim_spaces((const UTF8 *)r)));
                }
            }

            if (q)
            {
                p = q + 1;

                // Handle quoted named.
                //
                q = (UTF8*)strchr((char*)p, '"');
                if (q)
                {
                    *q = '\0';
                }

                dbref target = lookup_player(executor, p, true);
                if (target >= 0)
                {
                    aPlayers[nPlayers++] = target;
                }
                else if (target == AMBIGUOUS)
                {
                    notify(executor, tprintf(T("I found more than one \"%s\"."), trim_spaces((const UTF8 *)p)));
                }
                else
                {
                    notify(executor, tprintf(T("I don\'t recognize \"%s\"."), trim_spaces((const UTF8 *)p)));
                }

                if (q)
                {
                    p = q + 1;
                }
                else
                {
                    break;
                }
            }
            else
            {
                break;
            }
        }
    }
    else
    {
        // Need to decode the A_LASTPAGE.
        //
        dbref aowner;
        int   aflags;
        UTF8* pLastPage = atr_get("do_page.645", executor, A_LASTPAGE, &aowner, &aflags);

        string_token st(pLastPage, T(","));
        UTF8* p;
        for (p = st.parse(); p; p = st.parse())
        {
            dbref target = mux_atol(p);
            if (Good_obj(target)
                && (isPlayer(target) || has_flag(target, target, T("PUPPET"))))
            {
                aPlayers[nPlayers++] = target;
            }
            else
            {
                notify(executor, tprintf(T("I don\'t recognize #%d."), target));
                bModified = true;
            }
        }
        free_lbuf(pLastPage);
    }

    int nValid = nPlayers;

    // Remove duplicate dbrefs.
    //
    int i;
    for (i = 0; i < nPlayers - 1; i++)
    {
        if (aPlayers[i] != NOTHING)
        {
            int j;
            for (j = i + 1; j < nPlayers; j++)
            {
                if (aPlayers[j] == aPlayers[i])
                {
                    aPlayers[j] = NOTHING;
                    bModified = true;
                    nValid--;
                }
            }
        }
    }

    // If we are doing more than reporting, we have some other dbref
    // validation to do.
    //
    if (nargs == 2
        || arg1[0] != '\0')
    {
        for (i = 0; i < nPlayers; i++)
        {
            if (Good_obj(aPlayers[i])
                && !page_check(executor, aPlayers[i]))
            {
                aPlayers[i] = NOTHING;
                bModified = true;
                nValid--;
            }
        }
    }

    if (bModified)
    {
        // Our aPlayers could be different than the one encoded on A_LASTPAGE.
        // Update the database.
        //
        ITL itl;
        UTF8* pBuff = alloc_lbuf("do_page.lastpage");
        UTF8* pBufc = pBuff;
        ItemToList_Init(&itl, pBuff, &pBufc, 0, (UTF8)',');
        for (i = 0; i < nPlayers; i++)
        {
            if (Good_obj(aPlayers[i])
                && !ItemToList_AddInteger(&itl, aPlayers[i]))
            {
                break;
            }
        }
        ItemToList_Final(&itl);
        atr_add_raw(executor, A_LASTPAGE, pBuff);
        free_lbuf(pBuff);
    }

    // Verify that the recipient list isn't empty.
    //
    if (nValid == 0)
    {
        if (nargs == 1
            && arg1[0] == '\0')
        {
            notify(executor, T("You have not paged anyone."));
        }
        else
        {
            notify(executor, T("No one to page."));
        }
        return;
    }

    // Build a friendly representation of the recipient list.
    //
    UTF8* aFriendly = alloc_lbuf("do_page.friendly");
    UTF8* pFriendly = aFriendly;

    if (nValid > 1)
    {
        safe_chr('(', aFriendly, &pFriendly);
    }
    bool bFirst = true;
    for (i = 0; i < nPlayers; i++)
    {
        if (aPlayers[i] != NOTHING)
        {
            if (bFirst)
            {
                bFirst = false;
            }
            else
            {
                safe_copy_buf(T(", "), 2, aFriendly, &pFriendly);
            }
            safe_str(Moniker(aPlayers[i]), aFriendly, &pFriendly);
        }
    }
    if (nValid > 1)
    {
        safe_chr(')', aFriendly, &pFriendly);
    }
    *pFriendly = '\0';

    // We may be able to proceed directly to the reporting case.
    //
    if (nargs == 1
        && arg1[0] == '\0')
    {
        notify(executor, tprintf(T("You last paged %s."), aFriendly));
        free_lbuf(aFriendly);
        return;
    }

    // Build messages.
    //
    UTF8* omessage = alloc_lbuf("do_page.omessage");
    UTF8* imessage = alloc_lbuf("do_page.imessage");
    UTF8* omp = omessage;
    UTF8* imp = imessage;

    UTF8* pMessage;
    if (nargs == 1)
    {
        // 'page A' form.
        //
        pMessage = arg1;
    }
    else
    {
        // 'page A=', 'page =B', and 'page A=B' forms.
        //
        pMessage = arg2;
    }

    int pageMode;
    switch (*pMessage)
    {
    case '\0':
        pageMode = 1;
        break;

    case ':':
        pageMode = 2;
        pMessage++;
        while (pMessage[0] == ' ')
        {
            pMessage++;
        }
        break;

    case ';':
        pageMode = 3;
        pMessage++;
        break;

    case '"':
        pMessage++;

        // FALL THROUGH

    default:
        pageMode = 0;
    }

    UTF8* newMessage = modSpeech(executor, pMessage, true, (UTF8*)"page");
    if (newMessage)
    {
        pMessage = newMessage;
    }

    char noSpaceChars[] = { '\'', '#', ':' };
    bool noSpaceCharFound = false;
    
    switch (pageMode)
    {
    case 1:
        // 'page A=' form.
        //
        if (nValid == 1)
        {
            safe_tprintf_str(omessage, &omp, T("Sensorium (%s): %s sends a ping."), aFriendly, Moniker(executor));
            safe_tprintf_str(imessage, &imp, T("Sensorium (%s): %s sends a ping."), aFriendly, Moniker(executor));
        }
        else
        {
            safe_tprintf_str(omessage, &omp, T("Sensorium %s: %s sends a ping."), aFriendly, Moniker(executor));
            safe_tprintf_str(imessage, &imp, T("Sensorium %s: %s sends a group-ping."), aFriendly, Moniker(executor));
        }
        break;

    case 2:
        if (nValid > 1)
        {
            safe_tprintf_str(omessage, &omp, T("Sensorium %s: "), aFriendly);
            safe_tprintf_str(imessage, &imp, T("Sensorium %s: "), aFriendly);
        }
        else
        {
            safe_tprintf_str(omessage, &omp, T("Sensorium (%s): "), aFriendly);
            safe_tprintf_str(imessage, &imp, T("Sensorium (%s): "), aFriendly);
        }
        for (int i = 0; i < sizeof noSpaceChars; i++)
        {
            if (pMessage[0] == noSpaceChars[i])
            {
                noSpaceCharFound = true;
                break;
            }
        }
        safe_tprintf_str(omessage, &omp, T("%s%s%s"), Moniker(executor), noSpaceCharFound ? T("") : T(" "), pMessage);
        safe_tprintf_str(imessage, &imp, T("%s%s%s"), Moniker(executor), noSpaceCharFound ? T("") : T(" "), pMessage);
        break;

    case 3:
        if (nValid > 1)
        {
            safe_tprintf_str(omessage, &omp, T("Sensorium %s: "), aFriendly);
            safe_tprintf_str(imessage, &imp, T("Sensorium %s: "), aFriendly);
        }
        else
        {
            safe_tprintf_str(omessage, &omp, T("Sensorium (%s): "), aFriendly);
            safe_tprintf_str(imessage, &imp, T("Sensorium (%s): "), aFriendly);
        }
        safe_tprintf_str(omessage, &omp, T("%s%s"), Moniker(executor), pMessage);
        safe_tprintf_str(imessage, &imp, T("%s%s"), Moniker(executor), pMessage);
        break;

    default:
        if (nValid > 1)
        {
            safe_tprintf_str(omessage, &omp, T("Sensorium %s: "), aFriendly);
            safe_tprintf_str(imessage, &imp, T("Sensorium %s: "), aFriendly);
        }
        else
        {
            safe_tprintf_str(omessage, &omp, T("Sensorium (%s): "), aFriendly);
            safe_tprintf_str(imessage, &imp, T("Sensorium (%s): "), aFriendly);
        }
        safe_tprintf_str(omessage, &omp, T("%s says, \"%s\""), Moniker(executor), pMessage);
        safe_tprintf_str(imessage, &imp, T("%s says, \"%s\""), Moniker(executor), pMessage);
        break;
    }
    free_lbuf(aFriendly);

    // Send message to recipients.
    //
    for (i = 0; i < nPlayers; i++)
    {
        dbref target = aPlayers[i];
        if (target != NOTHING)
        {
            notify_with_cause_ooc(target, executor, omessage, MSG_SRC_PAGE);
            int target_idle = fetch_idle(target);
            int target_idle_timeout_val = idle_timeout_val(target);
            if (target_idle >= target_idle_timeout_val)
            {
                page_return(executor, target, T("Idle"), A_IDLE, nullptr);
            }
        }
    }
    free_lbuf(omessage);

    // Send message to sender.
    //
    notify(executor, imessage);
    free_lbuf(imessage);
    if (newMessage)
    {
        free_lbuf(newMessage);
    }
}

/* ---------------------------------------------------------------------------
 * do_pemit: Messages to specific players, or to all but specific players.
 */

static void whisper_pose(dbref player, dbref target, UTF8* message, bool bSpace, UTF8* from)
{
    UTF8* newMessage = modSpeech(player, message, true, (UTF8*)"whisper");
    if (newMessage)
    {
        message = newMessage;
    }
    UTF8* buff = alloc_lbuf("do_pemit.whisper.pose");
    mux_strncpy(buff, Moniker(player), LBUF_SIZE - 1);
    notify_with_cause(target, player, tprintf(T("Whisper (%s): %s%s%s"), from, Moniker(player), bSpace ? " " : "", message));
    free_lbuf(buff);
    if (newMessage)
    {
        free_lbuf(newMessage);
    }
}

static dbref FindPemitTarget(dbref player, int key, UTF8* recipient)
{
    dbref target = NOTHING;

    switch (key)
    {
    case PEMIT_FSAY:
    case PEMIT_FPOSE:
    case PEMIT_FPOSE_NS:
    case PEMIT_FEMIT:
        target = match_controlled(player, recipient);
        break;

    default:
        init_match(player, recipient, TYPE_PLAYER);
        match_everything(0);
        target = match_result();
    }
    return target;
}

void do_pemit_single
(
    dbref player,
    int key,
    bool bDoContents,
    int pemit_flags,
    dbref target,
    int chPoseType,
    UTF8* message,
    dbref* aPlayers = nullptr,
    int nPlayers = 0
)
{
    dbref loc;
    UTF8* buf2, * bp;
    int depth;
    bool ok_to_do = false;

    UTF8 *from, *bufc;
    from = bufc = alloc_lbuf("do_pemit_single.listfrom");
    safe_str(Moniker(player), from, &bufc);
    while (nPlayers > 0)
    {
        nPlayers--;
        if (aPlayers[nPlayers] == target)
        {
            continue;
        }

        safe_str(T(", "), from, &bufc);
        safe_str(Moniker(aPlayers[nPlayers]), from, &bufc);
    }
    *bufc = '\0';

    switch (key)
    {
    case PEMIT_FSAY:
    case PEMIT_FPOSE:
    case PEMIT_FPOSE_NS:
    case PEMIT_FEMIT:
        if (target == NOTHING)
        {
            return;
        }
        ok_to_do = true;
        break;
    }

    UTF8* newMessage = nullptr;
    UTF8* saystring = nullptr;

    UTF8* p;
    switch (target)
    {
    case NOTHING:
        switch (key)
        {
        case PEMIT_WHISPER:
            notify(player, T("Whisper to whom?"));
            break;

        case PEMIT_PEMIT:
            notify(player, T("Emit to whom?"));
            break;

        case PEMIT_OEMIT:
            notify(player, T("Emit except to whom?"));
            break;

        default:
            notify(player, T("Sorry."));
            break;
        }
        break;

    case AMBIGUOUS:
        notify(player, T("I don\'t know who you mean!"));
        break;

    default:

        // Enforce locality constraints.
        //
        if (!ok_to_do
            && (nearby(player, target)
                || Long_Fingers(player)
                || Controls(player, target)))
        {
            ok_to_do = true;
        }
        if (!ok_to_do
            && key == PEMIT_PEMIT
            && isPlayer(target)
            && mudconf.pemit_players)
        {
            if (!page_check(player, target))
            {
                return;
            }
            ok_to_do = true;
        }
        if (!ok_to_do
            && (!mudconf.pemit_any
                || key != PEMIT_PEMIT))
        {
            notify(player, T("You are too far away to do that."));
            return;
        }
        if (bDoContents
            && !Controls(player, target)
            && !mudconf.pemit_any)
        {
            notify(player, NOPERM_MESSAGE);
            return;
        }
        loc = where_is(target);

        char noSpaceChars[] = { '\'', '#', ':' };
        bool noSpaceCharFound = false;

        switch (key)
        {
        case PEMIT_PEMIT:
            if (bDoContents)
            {
                if (Has_contents(target))
                {
                    notify_all_from_inside(target, player, message);
                }
            }
            else
            {
                if (pemit_flags & PEMIT_HTML)
                {
                    notify_with_cause_html(target, player, message);
                }
                else
                {
                    notify_with_cause(target, player, message);
                }
            }
            break;

        case PEMIT_OEMIT:
            notify_except(Location(target), player, target, message, 0);
            break;

        case PEMIT_WHISPER:
            if (isPlayer(target)
                && !Connected(target))
            {
                page_return(player, target, T("Away"), A_AWAY,
                    tprintf(T("Sorry, %s is not connected."), Moniker(target)));
                return;
            }
            switch (chPoseType)
            {
            case ':':
                message++;
                while (message[0] == ' ')
                {
                    message++;
                }
                for (int i = 0; i < sizeof noSpaceChars; i++)
                {
                    if (message[0] == noSpaceChars[i])
                    {
                        noSpaceCharFound = true;
                        break;
                    }
                }
                whisper_pose(player, target, message, !noSpaceCharFound, from);
                break;

            case ';':
                message++;
                whisper_pose(player, target, message, false, from);
                break;

            case '"':
                message++;

            default:
                newMessage = modSpeech(player, message, true, (UTF8*)"whisper");
                if (newMessage)
                {
                    message = newMessage;
                }
                notify_with_cause(target, player,
                    tprintf(T("Whisper (%s): %s says, \"%s\""), from, Moniker(player), message));
                if (newMessage)
                {
                    free_lbuf(newMessage);
                }
            }
            if (!mudconf.quiet_whisper
                && !Wizard(player))
            {
                loc = where_is(player);
                if (loc != NOTHING)
                {
                    buf2 = alloc_lbuf("do_pemit.whisper.buzz");
                    bp = buf2;
                    safe_str(Moniker(player), buf2, &bp);
                    safe_str(T(" whispers something to "), buf2, &bp);
                    safe_str(Moniker(target), buf2, &bp);
                    *bp = '\0';
                    notify_except2(loc, player, player, target, buf2);
                    free_lbuf(buf2);
                }
            }
            break;

        case PEMIT_FSAY:
            newMessage = modSpeech(target, message, true, (UTF8*)"@fsay");
            if (newMessage)
            {
                message = newMessage;
            }
            notify(target, tprintf(T("%s says, \"%s\""), Moniker(player), message));
            if (loc != NOTHING)
            {
                saystring = modSpeech(target, message, false, (UTF8*)"@fsay");
                if (saystring)
                {
                    p = tprintf(T("%s %s \"%s\""), Moniker(target),
                        saystring, message);
                    notify_except(loc, player, target, p, 0);
                }
                else
                {
                    p = tprintf(T("%s says, \"%s\""), Moniker(target),
                        message);
                    notify_except(loc, player, target, p, 0);
                }
            }
            if (saystring)
            {
                free_lbuf(saystring);
            }
            if (newMessage)
            {
                free_lbuf(newMessage);
            }
            break;

        case PEMIT_FPOSE:
            newMessage = modSpeech(target, message, true, (UTF8*)"@fpose");
            if (newMessage)
            {
                message = newMessage;
            }
            for (int i = 0; i < sizeof noSpaceChars; i++)
            {
                if (message[0] == noSpaceChars[i])
                {
                    noSpaceCharFound = true;
                    break;
                }
            }
            p = tprintf(T("%s%s%s"), Moniker(target), noSpaceCharFound ? T("") : T(" "), message);
            notify_all_from_inside(loc, player, p);
            if (newMessage)
            {
                free_lbuf(newMessage);
            }
            break;

        case PEMIT_FPOSE_NS:
            newMessage = modSpeech(target, message, true, (UTF8*)"@fpose");
            if (newMessage)
            {
                message = newMessage;
            }
            p = tprintf(T("%s%s"), Moniker(target), message);
            notify_all_from_inside(loc, player, p);
            if (newMessage)
            {
                free_lbuf(newMessage);
            }
            break;

        case PEMIT_FEMIT:
            if ((pemit_flags & PEMIT_HERE)
                || !pemit_flags)
            {
                notify_all_from_inside(loc, player, message);
            }
            if (pemit_flags & PEMIT_ROOM)
            {
                if (isRoom(loc)
                    && (pemit_flags & PEMIT_HERE))
                {
                    return;
                }
                depth = 0;
                while (!isRoom(loc)
                    && depth++ < 20)
                {
                    loc = Location(loc);
                    if (loc == NOTHING
                        || loc == Location(loc))
                    {
                        return;
                    }
                }
                if (isRoom(loc))
                {
                    notify_all_from_inside(loc, player, message);
                }
            }
            break;
        }
    }
}

void do_pemit_single
(
    dbref player,
    int key,
    bool bDoContents,
    int pemit_flags,
    UTF8* recipient,
    int chPoseType,
    UTF8* message,
    dbref* aPlayers,
    int nPlayers
)
{
    dbref target = FindPemitTarget(player, key, recipient);
    do_pemit_single(player, key, bDoContents, pemit_flags, target, chPoseType, message, aPlayers, nPlayers);
}

void do_pemit_list
(
    dbref player,
    int key,
    bool bDoContents,
    int pemit_flags,
    UTF8* list,
    int chPoseType,
    UTF8* message
)
{
    if ('\0' == message[0]
        || '\0' == list[0])
    {
        return;
    }

    UTF8* error_message = nullptr;
    UTF8* error_ptr = nullptr;
    dbref aPlayers[(LBUF_SIZE + 1) / 2];
    int   nPlayers = 0;

    string_token st(list, T(","));
    for (UTF8* p = st.parse(); p; p = st.parse())
    {
        dbref target = FindPemitTarget(player, key, p);

        if (Good_obj(target))
        {
            aPlayers[nPlayers++] = target;
        }
        else
        {
            if (nullptr == error_message)
            {
                error_message = alloc_lbuf("do_pemit_list.error");
                error_ptr = error_message;
                safe_str(T("Emit error(s): "), error_message, &error_ptr);
            }
            else
            {
                safe_str(T(", "), error_message, &error_ptr);
            }

            safe_str(p, error_message, &error_ptr);

            switch (target)
            {
            case NOTHING:
                safe_str(T(" (unknown)"), error_message, &error_ptr);
                break;

            case AMBIGUOUS:
                safe_str(T(" (ambiguous)"), error_message, &error_ptr);
                break;
            }
        }
    }

    // Remove duplicate dbrefs.
    //
    int i;
    for (i = 0; i < nPlayers - 1; i++)
    {
        if (aPlayers[i] != NOTHING)
        {
            int j;
            for (j = i + 1; j < nPlayers; j++)
            {
                if (aPlayers[j] == aPlayers[i])
                {
                    aPlayers[j] = NOTHING;
                }
            }
        }
    }

    for (int i = 0; i < nPlayers; i++)
    {
        dbref target = aPlayers[i];
        if (NOTHING != target)
        {
            do_pemit_single(player, key, bDoContents, pemit_flags, target, chPoseType, message, aPlayers, nPlayers);
        }
    }

    if (nullptr != error_message)
    {
        *error_ptr = '\0';
        notify(player, error_message);
        free_lbuf(error_message);
        error_message = nullptr;
        error_ptr = nullptr;
    }
}

// Check if the target can be contacted by executor.   If not, issue an
// error message to the executor and return false.   If checks pass, return
// true.
//
static bool noisy_check_whisper_target(dbref executor, dbref target, int key)
{
    bool ok_to_do = false;

    if (nearby(executor, target)
        || Long_Fingers(executor)
        || Controls(executor, target))
    {
        ok_to_do = true;
    }

    if (!ok_to_do
        && (!mudconf.pemit_any
            || PEMIT_PEMIT != key))
    {
        notify(executor,
            tprintf(T("Sorry, you are to far away to contact %s."),
                Moniker(target)));
        return false;
    }

    if (isPlayer(target)
        && !Connected(target))
    {
        page_return(executor, target, T("Away"), A_AWAY,
            tprintf(T("Sorry, %s is not connected."),
                Moniker(target)));
        return false;
    }
    return true;
}

void do_pemit_whisper
(
    dbref executor,
    dbref caller,
    dbref enactor,
    int   key,
    int   nargs,
    UTF8* recipient,
    UTF8* message
)
{
    UNUSED_PARAMETER(caller);
    UNUSED_PARAMETER(enactor);

    // If the argument count is not 2, pull recipients from A_LASTWHISPER.
    //
    if (nargs < 2)
    {
        if ('\0' == recipient[0])
        {
            return;
        }
        message = recipient;
        recipient = nullptr;
    }

    bool  bModified = true;
    int   nPlayers = 0;
    dbref aPlayers[(LBUF_SIZE + 1) / 2];

    // Read the A_LASTWHISPER attribute and use that recipient list.
    //
    if (PEMIT_WHISPER == key
        && (nullptr == recipient
            || '\0' == recipient[0]))
    {
        dbref aowner;
        int   aflags;
        UTF8* stored_recipient =
            atr_get("do_whisper.1316", executor, A_LASTWHISPER,
                &aowner, &aflags);

        bModified = false;

        string_token st(stored_recipient, T(","));
        UTF8* r;
        for (r = st.parse(); r; r = st.parse())
        {
            dbref target = mux_atol(r);
            if (Good_obj(target))
            {
                if (!noisy_check_whisper_target(executor, target, key))
                {
                    continue;
                }
                else
                {
                    aPlayers[nPlayers++] = target;
                }
            }
        }

        free_lbuf(stored_recipient);
    }

    if (bModified)
    {
        UTF8* p = recipient;
        while ('\0' != *p)
        {
            UTF8* q = (UTF8*)strchr((char*)p, '"');
            if (q)
            {
                *q = '\0';
            }

            // Decode space-delimited or comma-delimited recipients.
            //
            string_token st(p, T(","));
            UTF8* r;
            for (r = st.parse(); r; r = st.parse())
            {
                dbref target = lookup_player(executor, r, true);

                if (NOTHING == target)
                {
                    init_match(executor, r, NOTYPE);
                    match_neighbor();
                    target = match_result();
                }

                if (target >= 0)
                {
                    if (!noisy_check_whisper_target(executor, target, key))
                    {
                        continue;
                    }
                    else
                    {
                        aPlayers[nPlayers++] = target;
                    }
                }
                else if (target == AMBIGUOUS)
                {
                    notify(executor, tprintf(T("I found more than one \"%s\"."), trim_spaces((const UTF8 *)r)));
                }
                else
                {
                    notify(executor, tprintf(T("I don\'t recognize \"%s\"."), trim_spaces((const UTF8 *)r)));
                }
            }

            if (q)
            {
                p = q + 1;

                // Handle quoted named.
                //
                q = (UTF8*)strchr((char*)p, '"');
                if (q)
                {
                    *q = '\0';
                }

                dbref target = lookup_player(executor, p, true);
                if (target >= 0)
                {
                    aPlayers[nPlayers++] = target;
                }
                else
                {
                    init_match(executor, p, NOTYPE);
                    match_neighbor();
                    target = match_result();

                    if (target >= 0)
                    {
                        if (!noisy_check_whisper_target(executor, target, key))
                        {
                            continue;
                        }
                        else
                        {
                            aPlayers[nPlayers++] = target;
                        }
                    }
                    else if (target == AMBIGUOUS)
                    {
                        notify(executor, tprintf(T("I found more than one \"%s\"!"), p));
                    }
                    else
                    {
                        notify(executor, tprintf(T("I did not find anyone named \"%s\"!"), p));
                    }
                }

                if (q)
                {
                    p = q + 1;
                }
                else
                {
                    break;
                }
            }
            else
            {
                break;
            }
        }

        int nValid = nPlayers;

        // Remove duplicate dbrefs.
        //
        int i;
        for (i = 0; i < nPlayers - 1; i++)
        {
            if (NOTHING != aPlayers[i])
            {
                int j;
                for (j = i + 1; j < nPlayers; j++)
                {
                    if (aPlayers[j] == aPlayers[i])
                    {
                        aPlayers[j] = NOTHING;
                        bModified = true;
                        nValid--;
                    }
                }
            }
        }

        if (nValid == 0)
        {
            notify(executor, T("Nobody to whisper to."));
            return;
        }
    }

    if (bModified
        && '\0' != recipient[0])
    {
        // Our aPlayers could be different than the one encoded on A_LASTWHISPER
        // Update the database.
        //
        ITL itl;
        UTF8* pBuff = alloc_lbuf("do_pemit_whisper.lastwhisper");
        UTF8* pBufc = pBuff;
        ItemToList_Init(&itl, pBuff, &pBufc, 0, (UTF8)',');
        for (int i = 0; i < nPlayers; i++)
        {
            if (Good_obj(aPlayers[i])
                && !ItemToList_AddInteger(&itl, aPlayers[i]))
            {
                break;
            }
        }
        ItemToList_Final(&itl);

        atr_add_raw(executor, A_LASTWHISPER, pBuff);
        free_lbuf(pBuff);
    }

    // Decode PEMIT_HERE, PEMIT_ROOM, PEMIT_HTML and remove from key.
    //
    int mask = PEMIT_HERE | PEMIT_ROOM | PEMIT_HTML;
    int pemit_flags = key & mask;
    key &= ~mask;

    int chPoseType = *message;

    char noSpaceChars[] = { '\'', '#', ':' };
    bool noSpaceCharFound = false;

    if (1 == nPlayers
        && Good_obj(aPlayers[0]))
    {
        switch (chPoseType)
        {
        case ';':
            notify(executor, tprintf(T("Whisper (%s): %s%s"), Moniker(aPlayers[0]), Moniker(executor), &message[1]));
            break;

        case ':':
            {
                int i = 1;
                while (message[i] == ' ')
                {
                    i++;
                }
                for (int i = 0; i < sizeof noSpaceChars; i++)
                {
                    if (message[0] == noSpaceChars[i])
                    {
                        noSpaceCharFound = true;
                        break;
                    }
                }
                notify(executor, tprintf(T("Whisper (%s): %s%s%s"), Moniker(aPlayers[0]), noSpaceCharFound ? T("") : T(" "), Moniker(executor), &message[i]));
                break;
            }

        default:
            notify(executor, tprintf(T("Whisper (%s): %s says, \"%s\""), Moniker(aPlayers[0]), Moniker(executor), message));
            break;
        }

    }
    else if (1 < nPlayers)
    {
        UTF8* aFriendly = alloc_lbuf("do_pemit_whisper.friendly");
        UTF8* pFriendly = aFriendly;

        bool bFirst = true;
        for (int i = 0; i < nPlayers; i++)
        {
            if (NOTHING != aPlayers[i])
            {
                if (bFirst)
                {
                    bFirst = false;
                }
                else if (nPlayers - 1 == i)
                {
                    safe_copy_buf(T(", "), 2, aFriendly, &pFriendly);
                }
                else
                {
                    safe_copy_buf(T(", "), 2, aFriendly, &pFriendly);
                }
                safe_str(Moniker(aPlayers[i]), aFriendly, &pFriendly);
            }
        }
        *pFriendly = '\0';

        switch (chPoseType)
        {
        case ';':
            notify(executor, tprintf(T("Whisper (%s): %s%s"), aFriendly, Moniker(executor), &message[1]));
            break;

        case ':':
            notify(executor, tprintf(T("Whisper (%s): %s%s"), aFriendly, Moniker(executor), &message[1]));
            break;

        default:
            notify(executor, tprintf(T("Whisper (%s): %s says, \"%s\""), aFriendly, Moniker(executor), message));
            break;
        }

        free_lbuf(aFriendly);
    }

    for (int i = 0; i < nPlayers; i++)
    {
        if (Good_obj(aPlayers[i]))
        {
            do_pemit_single(executor, key, false, pemit_flags,
                tprintf(T("#%d"), aPlayers[i]), chPoseType, message, aPlayers, nPlayers);
        }
    }
}


void do_pemit
(
    dbref executor,
    dbref caller,
    dbref enactor,
    int   eval,
    int   key,
    int   nargs,
    UTF8* recipient,
    UTF8* message,
    const UTF8* cargs[],
    int   ncargs
)
{
    UNUSED_PARAMETER(caller);
    UNUSED_PARAMETER(enactor);
    UNUSED_PARAMETER(eval);
    UNUSED_PARAMETER(cargs);
    UNUSED_PARAMETER(ncargs);

    if (nargs < 2
        && key != PEMIT_WHISPER)
    {
        return;
    }

    if (PEMIT_WHISPER == key)
    {
        do_pemit_whisper(executor, caller, enactor, key, nargs, recipient, message);
        return;
    }

    // Decode PEMIT_CONTENTS and PEMIT_LIST and remove from key.
    //
    bool bDoContents = false;
    if (key & PEMIT_CONTENTS)
    {
        bDoContents = true;
    }
    bool bDoList = false;
    if (key & (PEMIT_LIST | PEMIT_WHISPER))
    {
        bDoList = true;
    }
    key &= ~(PEMIT_CONTENTS | PEMIT_LIST);

    // Decode PEMIT_HERE, PEMIT_ROOM, PEMIT_HTML and remove from key.
    //
    int mask = PEMIT_HERE | PEMIT_ROOM | PEMIT_HTML;
    int pemit_flags = key & mask;
    key &= ~mask;

    int chPoseType = *message;
    if (PEMIT_WHISPER == key
        && ':' == chPoseType)
    {
        message[0] = ' ';
    }

    if (bDoList)
    {
        do_pemit_list(executor, key, bDoContents, pemit_flags, recipient,
            chPoseType, message);
    }
    else
    {
        do_pemit_single(executor, key, bDoContents, pemit_flags, recipient,
            chPoseType, message);
    }
}
