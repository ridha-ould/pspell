/* enchant
 * Copyright (C) 2003 Dom Lachowicz
 *               2007 Hannu Väisänen
 *               2016-2017 Reuben Thomas
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.	 See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the
 * Free Software Foundation, Inc., 59 Temple Place - Suite 330,
 * Boston, MA 02110-1301, USA.
 *
 * In addition, as a special exception, Dom Lachowicz
 * gives permission to link the code of this program with
 * the non-LGPL Spelling Provider libraries (eg: a MSFT Office
 * spell checker backend) and distribute linked combinations including
 * the two.  You must obey the GNU Lesser General Public License in all
 * respects for all of the code used other than said providers. If you modify
 * this file, you may extend this exception to your version of the
 * file, but you are not obligated to do so. If you do not wish to
 * do so, delete this exception statement from your version.
 */

/**
 * This is an ispell-compatible command-line version of Enchant.
 */

#include "config.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <locale.h>
#include <glib.h>
#include <glib/gstdio.h>
#ifdef _WIN32
#include <windows.h>
#endif

#include "enchant.h"
#include "enchant-provider.h"


/* word has to be bigger than this to be checked */
#define MIN_WORD_LENGTH 1

static const char *charset;

typedef enum 
	{
		MODE_NONE,
		MODE_VERSION,
		MODE_A,
		MODE_L
	} IspellMode_t;

static void 
print_version (FILE * to)
{
	fprintf (to, "@(#) International Ispell Version 3.1.20 (but really Enchant %s)\n", PACKAGE_VERSION);
	fflush (to);
}

static void
print_help (FILE * to, const char * prog)
{
	fprintf (to,
		 "Usage: %s [OPTION...] FILE\n\
  -a lists suggestions in ispell pipe mode format\n\
  -d DICTIONARY uses the given dictionary\n\
  -h Show this help message\n\
  -l lists misspellings\n\
  -L displays line numbers\n\
  -v displays program version.\n", prog);
}

static gboolean
consume_line (FILE * in, GString * str)
{
	int ch;
	gsize bytes_read, bytes_written;
	gchar * utf;
	gboolean ret = TRUE;

	g_string_truncate (str, 0);

	while (ret && (ch = fgetc (in)) != EOF) {
		if (ch == '\r')
			continue;
		else {
			if (ch == '\n')
				ret = FALSE;
			else
				g_string_append_c (str, ch);
		}
	}

	if (str->len) {
		utf = g_convert(str->str, str->len, "UTF-8", charset, &bytes_read, &bytes_written, NULL);
		if (utf) {
			g_string_assign (str, utf);
			g_free (utf);
		} 
		/* Else str->str stays the same. we'll assume that it's 
		   already utf8 and glib is just being stupid. */
	}

	return ret;
}

static void
print_utf (FILE * out, const char * str)
{
	gsize bytes_read, bytes_written;
	gchar * native;

	native = g_locale_from_utf8 (str, -1, &bytes_read, &bytes_written, NULL);
	if (native) {
		fwrite (native, 1, bytes_written, out);
		g_free (native);
	} else {
		/* We'll assume that it's already utf8 and glib is just being stupid. */
		fwrite (str, 1, strlen (str), out);
	}
}

static void
do_mode_a (FILE * out, EnchantDict * dict, GString * word, size_t start_pos, size_t lineCount, gboolean terse_mode)
{
	size_t n_suggs;
	char ** suggs;	

	if (word->len <= MIN_WORD_LENGTH || enchant_dict_check (dict, word->str, word->len) == 0) {
		if (!terse_mode) {
			if (lineCount)
				fprintf (out, "* %u\n", (unsigned int)lineCount);
			else
				fwrite ("*\n", 1, 2, out);
		}
	}
	else {
		suggs = enchant_dict_suggest (dict, word->str, 
					      word->len, &n_suggs);
		if (!n_suggs || !suggs) {
			fwrite ("# ", 1, 2, out);
			if (lineCount)
				fprintf (out, "%u ", (unsigned int)lineCount);
			print_utf (out, word->str);
			fprintf (out, " %u\n", (unsigned int)start_pos);
		}
		else {
			size_t i = 0;
			
			fwrite ("& ", 1, 2, out);
			if (lineCount)
				fprintf (out, "%u ", (unsigned int)lineCount);
			print_utf (out, word->str);
			fprintf (out, " %u %u:", (unsigned int)n_suggs, (unsigned int)start_pos);
			
			for (i = 0; i < n_suggs; i++) {
				fprintf (out, " ");
				print_utf (out, suggs[i]);

				if (i != (n_suggs - 1))
					fwrite (",", 1, 1, out);
				else
					fwrite ("\n", 1, 1, out);
			}

			enchant_dict_free_string_list (dict, suggs);
		}
	}
}

static void
do_mode_l (FILE * out, EnchantDict * dict, GString * word, size_t lineCount)
{
	if (enchant_dict_check (dict, word->str, word->len) != 0) {
		if (lineCount)
			fprintf (out, "%u ", (unsigned int)lineCount);
		print_utf (out, word->str);
		fwrite ("\n", 1, 1, out);
	}
}


/* Splits a line into a set of (word,word_position) tuples. */
static GSList *
tokenize_line (EnchantDict * dict, GString * line)
{
	GSList * tokens = NULL;
	char *utf = (char *) line->str;

	GString * word;
	
	gunichar uc;
	size_t cur_pos = 0;
	size_t start_pos = 0;
	word = g_string_new (NULL);

	while (cur_pos < line->len && *utf) {
		int i;

	        /* Skip non-word characters. */
		cur_pos = g_utf8_pointer_to_offset ((const char*)line->str, utf);
		uc = g_utf8_get_char (utf);
		while (cur_pos < line->len && *utf && !enchant_dict_is_word_character (dict, uc, 0)) {
		        utf = g_utf8_next_char (utf);
			uc = g_utf8_get_char (utf);
			cur_pos = g_utf8_pointer_to_offset ((const char*)line->str, utf);
		}
		start_pos = cur_pos;

		/* Skip over word characters. */
		while (cur_pos < line->len && *utf && enchant_dict_is_word_character (dict, uc, 1)) {
			g_string_append_unichar (word, uc);
		        utf = g_utf8_next_char (utf);
			uc = g_utf8_get_char (utf);
			cur_pos = g_utf8_pointer_to_offset ((const char*)line->str, utf);
		}

	        /* Skip backwards over any characters that can't appear at the end of a word. */
		i = word->len-1;
	        while ((i >= 0) && !enchant_dict_is_word_character(dict, word->str[i], 2)) {
	                g_string_truncate (word, i);
			i--;
		}

		/* Save (word, position) tuple. */
                if (word->len) {
		        tokens = g_slist_append (tokens, g_string_new_len (word->str, word->len));
			tokens = g_slist_append (tokens, GINT_TO_POINTER(start_pos));
			g_string_truncate (word, 0);
		}
	}
	g_string_free (word, TRUE);

	return tokens;
}

static int
parse_file (FILE * in, FILE * out, IspellMode_t mode, int countLines, gchar *dictionary)
{
	EnchantBroker * broker;
	EnchantDict * dict;
	
	GString * str, * word = NULL;
	GSList * tokens, *token_ptr;
	gchar * lang;
	size_t pos, lineCount = 0;

	gboolean was_last_line = FALSE, corrected_something = FALSE, terse_mode = FALSE;

	if (mode == MODE_A)
		print_version (out);

	if (dictionary)
		lang = strdup (dictionary);
	else {
	        lang = enchant_get_user_language();
		if(!lang)
			return 1;
 	}

	/* Enchant will get rid of trailing information like de_DE@euro or de_DE.ISO-8859-15 */
	
	broker = enchant_broker_init ();
	dict = enchant_broker_request_dict (broker, lang);

	if (!dict) {
		fprintf (stderr, "Couldn't create a dictionary for %s\n", lang);
		free (lang);
		enchant_broker_free (broker);
		return 1;
	}

	free (lang);

	str = g_string_new (NULL);
	
	while (!was_last_line) {
		gboolean mode_A_no_command = FALSE;
		was_last_line = consume_line (in, str);

		if (countLines)
			lineCount++;

		if (str->len) {
			corrected_something = FALSE;

			if (mode == MODE_A) {
				switch (*str->str) {
				case '&': /* Insert uncapitalised in personal word list */
					if (str->len > 1) {
						gunichar c = g_utf8_get_char_validated(str->str + 1, str->len);
						if (c > 0) {
							str = g_string_erase(str, 1, g_utf8_next_char(str->str + 1) - (str->str + 1));
							g_string_insert_unichar(str, 1, g_unichar_tolower(c));
						}
					}
					/* FALLTHROUGH */
				case '*': /* Insert in personal word list */
					if (str->len == 1)
						goto empty_word;
					enchant_dict_add(dict, str->str + 1, -1);
					break;
				case '@': /* Accept for this session */
					if (str->len == 1)
						goto empty_word;
					enchant_dict_add_to_session(dict, str->str + 1, -1);
					break;
				case '/': /* Remove from personal word list */
					if (str->len == 1)
						goto empty_word;
					enchant_dict_remove (dict, str->str + 1, -1);
					break;
				case '_': /* Remove from this session */
					if (str->len == 1)
						goto empty_word;
					enchant_dict_remove_from_session (dict, str->str + 1, -1);
					break;

				case '%': /* Exit terse mode */
					terse_mode = FALSE;
					break;
				case '!': /* Enter terse mode */
					terse_mode = TRUE;
					break;

				/* Ignore these commands */
				case '#': /* Save personal word list (enchant does this automatically) */
				case '+': /* LaTeX mode */
				case '-': /* nroff mode [default] */
				case '~': /* change string character type (enchant is fixed to UTF-8) */
				case '`': /* Enter verbose-correction mode */
					break;

				case '$': /* Miscellaneous commands */
					{
						const gchar *prefix = "$$ra "; /* Save correction for rest of session [aspell extension] */
						if (g_str_has_prefix(str->str, prefix)) { /* Syntax: $$ra <MISSPELLED>,<REPLACEMENT> */
							gchar *comma = g_utf8_strchr(str->str, -1, (gunichar)',');
							char *mis = str->str + strlen(prefix);
							char *cor = comma + 1;
							ssize_t mis_len = comma - mis;
							ssize_t cor_len = strlen(str->str) - (cor - str->str);
							enchant_dict_store_replacement(dict, mis, mis_len, cor, cor_len);
						} else if (g_str_has_prefix(str->str, "$$wc")) { /* Return the extra word chars list */
							fprintf(out, "%s\n", enchant_dict_get_extra_word_characters(dict));
						}
					}
					break;

				case '^': /* ^ is used as prefix to prevent interpretation of original
					     first character as a command */
					/* FALLTHROUGH */
				default: /* A word or words to check */
					mode_A_no_command = TRUE;
					break;

				empty_word:
					fprintf (out, "Error: The word \"\" is invalid. Empty string.\n");
				}
			}

			if (mode != MODE_A || mode_A_no_command) {
				token_ptr = tokens = tokenize_line (dict, str);
				if (tokens == NULL)
					putc('\n', out);
				while (tokens != NULL) {
					corrected_something = TRUE;

					word = (GString *)tokens->data;
					tokens = tokens->next;
					pos = GPOINTER_TO_INT(tokens->data);
					tokens = tokens->next;

					if (mode == MODE_A)
						do_mode_a (out, dict, word, pos, lineCount, terse_mode);
					else if (mode == MODE_L)
						do_mode_l (out, dict, word, lineCount);

					g_string_free(word, TRUE);
				}
				if (token_ptr)
					g_slist_free (token_ptr);
			}
		} 
		
		if (mode == MODE_A && corrected_something) {
			fwrite ("\n", 1, 1, out);
		}
		g_string_truncate (str, 0);
		fflush (out);
	}

	enchant_broker_free_dict (broker, dict);
	enchant_broker_free (broker);

	g_string_free (str, TRUE);

	return 0;
}

int main (int argc, char ** argv)
{
	IspellMode_t mode = MODE_NONE;
	
	char * file = NULL;
	int i, rval = 0;
	
	FILE * fp = stdin;

	int countLines = 0;
	gchar *dictionary = NULL;  /* -d dictionary */

	/* Initialize system locale */
	setlocale(LC_ALL, "");

	g_get_charset(&charset);
#ifdef _WIN32
	/* If reading from stdin, its CP may not be the system CP (which glib's locale gives us) */
	if (GetFileType(GetStdHandle(STD_INPUT_HANDLE)) == FILE_TYPE_CHAR) {
		charset = g_strdup_printf("CP%u", GetConsoleCP());
	}
#endif

	for (i = 1; i < argc; i++) {
		char * arg = argv[i];
		if (arg[0] == '-') {
			if (strlen (arg) == 2) {
				/* It seems that the first one of these that is specified gets precedence. */
				if (arg[1] == 'a' && MODE_NONE == mode)
					mode = MODE_A;
				else if (arg[1] == 'l' && MODE_NONE == mode)
					mode = MODE_L;
				else if (arg[1] == 'v' && MODE_NONE == mode)
					mode = MODE_VERSION;
				else if (arg[1] == 'L' && MODE_NONE == mode)
					countLines = 1;
				else if (arg[1] == 'm')
				     	; /* Ignore. Emacs calls ispell with '-m'. */
				else if (arg[1] == 'd') {
				     	i++;
					dictionary = argv[i];  /* Emacs calls ispell with '-d dictionary'. */
				}
			} 
			else if ((strlen (arg) == 3) && (arg[1] == 'v') && (arg[2] == 'v')) {
				mode = MODE_VERSION;   /* Emacs calls ispell with '-vv'. */
			}
			else if (arg[1] == 'd') {
			        dictionary = arg + 2;  /* Accept "-ddictionary", i.e. no space between -d and dictionary. */
			}
			else if (strlen (arg) > 2) {
				fprintf (stderr, "-%c does not take any parameters.\n", arg[1]);
				exit(1);
			} 
			else
				file = arg;
		} 
		else
			file = arg;
	}
	
	if (mode == MODE_VERSION) {
		print_version (stdout);
	} 
	else if (mode == MODE_NONE && !file) {
		print_help (stdout, argv[0]);
	}
	else {
		if (file) {
			fp = g_fopen (file, "rb");
			if (!fp) {
				fprintf (stderr, "Error: Could not open the file \"%s\" for reading.\n", file);
				exit (1);
			}
		}
		
		rval = parse_file (fp, stdout, mode, countLines, dictionary);
		
		if (file)
			fclose (fp);
	}
	
	return rval;
}
