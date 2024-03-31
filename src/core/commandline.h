/*
 * Open Surge Engine
 * commandline.h - command line parser
 * Copyright 2008-2024 Alexandre Martins <alemartf(at)gmail.com>
 * http://opensurge2d.org
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */

#ifndef _COMMANDLINE_H
#define _COMMANDLINE_H

#define COMMANDLINE_PATHMAX 4096

/* command line structure */
typedef struct commandline_t commandline_t;
struct commandline_t
{
    /* video options */
    int video_resolution;
    int video_quality;
    int fullscreen;
    int show_fps;
    int hide_fps;

    /* other options */
    int mobile;
    int verbose;
    int compatibility_mode;
    char compatibility_version[16];

    /* filepaths */
    char gamedir[COMMANDLINE_PATHMAX];
    char custom_level_path[COMMANDLINE_PATHMAX];
    char custom_quest_path[COMMANDLINE_PATHMAX];
    char language_filepath[COMMANDLINE_PATHMAX];

    /* user arguments: what comes after "--" */
    const char** user_argv;
    int user_argc;

    /* argv[0] as passed to main() */
    char argv0[COMMANDLINE_PATHMAX];
};



/* parse the command line arguments */
commandline_t commandline_parse(int argc, char **argv);

/* get an int from the command line arguments */
int commandline_getint(int value, int default_value);

/* get a string from the command line arguments */
const char* commandline_getstring(const char* value, const char* default_string);



#endif
