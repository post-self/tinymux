---
author: Brazil
date: February 2021
title: INSTALL
---

Please note that there are two sets of instructions included in this
file.  Please skip to _Instructions for Existing Games_ for how to
upgrade your server or to compile in preparation for moving an existing
game.

# Prerequisites

TinyMUX requires PCRE2. On Ubuntu, you can install this with

    sudo apt install libpcre2-dev

# Instructions for New Installations:

1.  **Prepare:** `cd src/` to the source directory.  Run `./configure`.

    This will customize `autoconf.h` and `Makefile` for your system.
    Optional packages are documented separately and enabled with the
    following configuration options:

      |                        |                                                  |
      |------------------------|--------------------------------------------------|
      | `--enable-memorybased` | See docs/MEMORY.                                 |
      | `--enable-realitylvls` | See REALITY and REALITY.SETUP.                   |
      | `--enable-stubslave`   | See MODULES.                                     |
      | `--enable-wodrealms`   | See docs/REALMS.                                 |
      | `--enable-inlinesql`   | Enables in-line MySQL support.                   |
      | `--enable-ssl`         | See SSL.                                         |
      | `--enable-deprecated`  | Enables deprecated features.                     |

2.  **Configure:** *(optional)* Edit the configuration section of the `Makefile`.  This is usually not
    needed.  Most likely, all you will need to change are any C++ flags
    needed by your particular C++ compiler, (in particular
    `-fpcc-struct-return`), and any esoteric libraries needed by your
    system.  There may also be some `#defines` in `config.h` that you may
    want to change, but in general, the defaults should not be changed.

3.  **Build:** Run `make depend`, then `make`.  This will produce `netmux`, `slave`, and
    `dbconvert`.

4.  **Initialize:** When starting from a TinyMUX for the first time, do the following:

      - cd to the game directory.  `cd ../game`
      - *(optional, but likely)* Make any changes to your configuration file that you may need, as described in `docs/CONFIGURATION`, such as changing the `GAMENAME`
      - Type `./Startmux`.  TinyMUX 2.13 will automatically create a minimal DB
        if one does not exist in the `game/data` directory
      - Log into the game as player wizard `connect wizard potrzebie` and
        shut it down again

5.  **Edit:** Edit the .txt files in `game/text` to your liking.  In particular,
    `connect.txt` and `motd.txt`.

6.  **Start:** Start TinyMUX 2.13 by running `./Startmux` again.

7.  **Create channels:** `@ccreate` a channel named `Public`, and a channel named `Guests`
    from within the TinyMUX.  Created players will automatically be
    joined to `Public` with alias `pub`, guests will automatically join
    `Guests` with alias `g`.

# Instructions for Existing Games:

NOTE: It is HIGHLY recommended that you preserve an earlier setup if you
can, to make conversion a bit less painful.  If you had one while
converting, make sure the conversion process has completed successfully
before you delete your old distribution.  We cannot stress enough to you
the importance of protecting your data throughout any conversion or
upgrade.

In a **new check-out of the code:**

1.  Follow steps **1–3** in the installation instructions above.

2.  Place/change your existing files from your previous build.

    - Put databases in `game/data`.

    - Put text files in `game/text`.

    - Update `mux.config` with any changes you made:

        - If you changed the `GAMENAME` in `mux.config`, be sure to change the
          filenames in `GAMENAME.conf` as well.

        - If you had a mail database previously, adjust `mail_expiration`
          accordingly, BEFORE you restart the game, or else ALL `@mail` older
          than the default value of 14 days will be deleted.

5.  Start TinyMUX 2.13 by running `./Startmux`.


# Changes to dbconvert:

 - `dbconvert` is the means by which the binary game data is converted to
   flatfile format and back again.  The `db_load` and `db_unload` scripts
   simplify the process for the user.

 - The syntax of the `db_load` script is:

```
       ./db_load netmux netmux.flat netmux.db
```

 - This converts a flatfile database to binary for use by the server
   and would be done with `dbconvert` thus:

```
       ../bin/dbconvert -dnetmux -inetmux.flat -onetmux.db -l
```

 - The syntax of the `db_unload` script is:

``` 
       ./db_unload netmux netmux.db.new netmux.flat
```

 - This converts binary data to flatfile for would be done with
   `dbconvert` thus:

```
       ../bin/dbconvert -dnetmux -inetmux.db.new -onetmux.flat -u
```
