/* Copyright (C) 2004 MySQL AB
   Copyright (C) 2004-2018 Alexey Kopytov <akopytov@gmail.com>

   This program is free software; you can redistribute it and/or modify
   it under the terms of the GNU General Public License as published by
   the Free Software Foundation; either version 2 of the License, or
   (at your option) any later version.

   This program is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
   GNU General Public License for more details.

   You should have received a copy of the GNU General Public License
   along with this program; if not, write to the Free Software
   Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA
*/

#ifdef HAVE_CONFIG_H
# include "config.h"
#endif

#ifdef STDC_HEADERS
# include <stdio.h>
# include <stdarg.h>
# include <string.h>
#endif
#ifdef HAVE_ERRNO_H
# include <errno.h>
#endif
#ifdef HAVE_MATH_H
# include <math.h>
#endif

#include "sysbench.h"
#include "sb_list.h"
#include "sb_logger.h"
#include "sb_histogram.h"

#include "ck_cc.h"

#define TEXT_BUFFER_SIZE 4096
#define ERROR_BUFFER_SIZE 256

/*
   Use 1024-element array for latency histogram tracking values between 0.001
   milliseconds and 100 seconds.
*/
#define OPER_LOG_GRANULARITY 1024
#define OPER_LOG_MIN_VALUE   1e-3
#define OPER_LOG_MAX_VALUE   1E5

/* Array of message handlers (one chain per message type) */

static sb_list_t handlers[LOG_MSG_TYPE_MAX];

/* set after logger initialization */
static unsigned char initialized; 

static pthread_mutex_t text_mutex;
static unsigned int    text_cnt;
static char            text_buf[TEXT_BUFFER_SIZE];


static int text_handler_init(void);
static int text_handler_process(log_msg_t *msg);

static int oper_handler_init(void);
static int oper_handler_done(void);

/* Built-in log handlers */

/* Text messages handler */

static sb_arg_t text_handler_args[] =
{
  SB_OPT("verbosity", "verbosity level {5 - debug, 0 - only critical messages}",
         "3", INT),

  SB_OPT_END
};

static log_handler_t text_handler = {
  {
    &text_handler_init,
    &text_handler_process,
    NULL,
  },
  text_handler_args,
  {0,0}
};

/* Operation start/stop messages handler */

static sb_arg_t oper_handler_args[] =
{
  SB_OPT("percentile", "list of percentiles to calculate in latency statistics (0-100). "
         "Use an empty list to disable percentile calculations",
         "95", LIST),
  SB_OPT("histogram", "print latency histogram in report", "off", BOOL),

  SB_OPT_END
};

static log_handler_t oper_handler = {
  {
    oper_handler_init,
    NULL,
    oper_handler_done,
  },
  oper_handler_args,
  {0,0}
};


/* Register logger and all handlers */


int log_register(void)
{
  unsigned int i;

  for (i = 0; i < LOG_MSG_TYPE_MAX; i++)
    SB_LIST_INIT(handlers + i);

  log_add_handler(LOG_MSG_TYPE_TEXT, &text_handler);
  log_add_handler(LOG_MSG_TYPE_OPER, &oper_handler);

  return 0;
}


/* Display command line options for registered log handlers */


void log_print_help(void)
{
  unsigned int    i;
  sb_list_item_t  *pos;
  log_handler_t   *handler;

  printf("Log options:\n");

  for (i = 0; i < LOG_MSG_TYPE_MAX; i++)
  {
    SB_LIST_FOR_EACH(pos, handlers + i)
    {
      handler = SB_LIST_ENTRY(pos, log_handler_t, listitem);
      if (handler->args != NULL)
	sb_print_options(handler->args);
    }
  }
}


/* Initialize logger and all handlers */


int log_init(void)
{
  unsigned int    i;
  sb_list_item_t  *pos;
  log_handler_t   *handler;

  for (i = 0; i < LOG_MSG_TYPE_MAX; i++)
  {
    SB_LIST_FOR_EACH(pos, handlers + i)
    {
      handler = SB_LIST_ENTRY(pos, log_handler_t, listitem);
      if (handler->ops.init != NULL && handler->ops.init())
        return 1;
    }
  }

  /* required to let log_text() pass messages to handlers */
  initialized = 1; 
  
  return 0;
}


/* Uninitialize logger and all handlers */


void log_done(void)
{
  unsigned int    i;
  sb_list_item_t  *pos;
  log_handler_t   *handler;

  for (i = 0; i < LOG_MSG_TYPE_MAX; i++)
  {
    SB_LIST_FOR_EACH(pos, handlers + i)
    {
      handler = SB_LIST_ENTRY(pos, log_handler_t, listitem);
      if (handler->ops.done != NULL)
        handler->ops.done();
    }
  }

  initialized = 0;
}  


/* Add handler for a specified type of messages */


int log_add_handler(log_msg_type_t type, log_handler_t *handler)
{
  if (type <= LOG_MSG_TYPE_MIN || type >= LOG_MSG_TYPE_MAX)
    return 1;

  if (handler->args != NULL)
    sb_register_arg_set(handler->args);
  
  SB_LIST_ADD_TAIL(&handler->listitem, handlers + type);

  return 0;
}


/* Main function to dispatch log messages */


void log_msg(log_msg_t *msg)
{
  sb_list_item_t  *pos;
  log_handler_t   *handler;
  
  SB_LIST_FOR_EACH(pos, handlers + msg->type)
  {
    handler = SB_LIST_ENTRY(pos, log_handler_t, listitem);
    if (handler->ops.process != NULL)
      handler->ops.process(msg);
  }
}

static const char *get_msg_prefix(log_msg_priority_t priority)
{
  const char * prefix;

  switch (priority) {
    case LOG_FATAL:
      prefix = "FATAL: ";
      break;
    case LOG_ALERT:
      prefix = "ALERT: ";
      break;
    case LOG_WARNING:
      prefix = "WARNING: ";
      break;
    case LOG_DEBUG:
      prefix = "DEBUG: ";
      break;
    default:
      prefix = "";
      break;
  }

  return prefix;
}

/* printf-like wrapper to log text messages */


void log_text(log_msg_priority_t priority, const char *fmt, ...)
{
  log_msg_t      msg;
  log_msg_text_t text_msg;
  char           buf[TEXT_BUFFER_SIZE];
  va_list        ap;
  int            n, clen, maxlen;

  maxlen = TEXT_BUFFER_SIZE;
  clen = 0;

  va_start(ap, fmt);
  n = vsnprintf(buf + clen, maxlen, fmt, ap);
  va_end(ap);
  if (n < 0 || n >= maxlen)
    n = maxlen;
  clen += n;
  maxlen -= n;
  snprintf(buf + clen, maxlen, "\n");

  /*
    No race condition here because log_init() is supposed to be called
    in a single-threaded stage
  */
  if (!initialized)
  {
    printf("%s%s", get_msg_prefix(priority), buf);

    return;
  }

  msg.type = LOG_MSG_TYPE_TEXT;
  msg.data = (void *)&text_msg;
  text_msg.priority = priority;
  text_msg.text = buf;
  text_msg.flags = 0;

  log_msg(&msg);
}


/*
  variant of log_text() which prepends log lines with the elapsed time of a
  specified timer.
*/


void log_timestamp(log_msg_priority_t priority, double seconds,
                   const char *fmt, ...)
{
  log_msg_t      msg;
  log_msg_text_t text_msg;
  char           buf[TEXT_BUFFER_SIZE];
  va_list        ap;
  int            n, clen, maxlen;

  maxlen = TEXT_BUFFER_SIZE;
  clen = 0;

  n = snprintf(buf, maxlen, "[ %.0fs ] ", seconds);
  clen += n;
  maxlen -= n;

  va_start(ap, fmt);
  n = vsnprintf(buf + clen, maxlen, fmt, ap);
  va_end(ap);
  if (n < 0 || n >= maxlen)
    n = maxlen;
  clen += n;
  maxlen -= n;
  snprintf(buf + clen, maxlen, "\n");

  /*
    No race condition here because log_init() is supposed to be called
    in a single-threaded stage
  */
  if (!initialized)
  {
    printf("%s%s", get_msg_prefix(priority), buf);

    return;
  }

  msg.type = LOG_MSG_TYPE_TEXT;
  msg.data = (void *)&text_msg;
  text_msg.priority = priority;
  text_msg.text = buf;
  /* Skip duplicate checks */
  text_msg.flags = LOG_MSG_TEXT_ALLOW_DUPLICATES;

  log_msg(&msg);
}


/* printf-like wrapper to log system error messages */


void log_errno(log_msg_priority_t priority, const char *fmt, ...)
{
  char           buf[TEXT_BUFFER_SIZE];
  char           errbuf[ERROR_BUFFER_SIZE];
  va_list        ap;
  int            n;
  int            old_errno;
  char           *tmp;

  old_errno = errno;
#ifdef HAVE_STRERROR_R
#ifdef STRERROR_R_CHAR_P
  tmp = strerror_r(old_errno, errbuf, sizeof(errbuf));
#else
  strerror_r(old_errno, errbuf, sizeof(errbuf));
  tmp = errbuf;
#endif /* STRERROR_R_CHAR_P */
#else /* !HAVE_STRERROR_P */
  strncpy(errbuf, strerror(old_errno), sizeof(errbuf));
  tmp = errbuf;
#endif /* HAVE_STRERROR_P */

  va_start(ap, fmt);
  n = vsnprintf(buf, TEXT_BUFFER_SIZE, fmt, ap);
  va_end(ap);
  if (n < 0 || n == TEXT_BUFFER_SIZE)
    return;
  snprintf(buf + n, TEXT_BUFFER_SIZE - n, " errno = %d (%s)", old_errno,
           tmp);

  log_text(priority, "%s", buf);
}



/* Initialize text handler */


int text_handler_init(void)
{
#ifdef HAVE_SETVBUF
  /* Set stdout to unbuffered mode */
  setvbuf(stdout, NULL, _IONBF, 0);
#endif
  
  sb_globals.verbosity = sb_get_value_int("verbosity");

  if (sb_globals.verbosity > LOG_DEBUG)
  {
    printf("Invalid value for verbosity: %d\n", sb_globals.verbosity);
    return 1;
  }

  pthread_mutex_init(&text_mutex, NULL);
  text_cnt = 0;
  text_buf[0] = '\0';
  
  return 0;
}


/* Print text message to the log */


int text_handler_process(log_msg_t *msg)
{
  log_msg_text_t *text_msg = (log_msg_text_t *)msg->data;

  if (text_msg->priority > sb_globals.verbosity)
    return 0;

  if (!(text_msg->flags & LOG_MSG_TEXT_ALLOW_DUPLICATES))
  {
    pthread_mutex_lock(&text_mutex);
    if (!strcmp(text_buf, text_msg->text))
    {
      text_cnt++;
      pthread_mutex_unlock(&text_mutex);

      return 0;
    }
    else
    {
      if (text_cnt > 0)
        printf("(last message repeated %u times)\n", text_cnt);

      text_cnt = 0;
      strncpy(text_buf, text_msg->text, TEXT_BUFFER_SIZE);
    }
    pthread_mutex_unlock(&text_mutex);
  }

  printf("%s%s", get_msg_prefix(text_msg->priority), text_msg->text);

  return 0;
}


/* Initialize operation messages handler */


int oper_handler_init(void)
{
  sb_list_t           *tmp;
  uint64_t            n = 0, i = 0;

  tmp = sb_get_value_list("percentile");

  sb_list_item_t *pos;
  SB_LIST_FOR_EACH(pos, tmp){
    value_t *val = SB_LIST_ENTRY(pos, value_t, listitem);
    double res = atof(val->data);

    if (res < 0 || res > 100)
    {
      log_text(LOG_FATAL, "Invalid value for --percentile: %f",
              res);
      return 1;
    }
    n++;
  }

  sb_globals.percentiles = malloc(n * sizeof(double));
  sb_globals.npercentiles = n;

  SB_LIST_FOR_EACH(pos, tmp){
    value_t *val = SB_LIST_ENTRY(pos, value_t, listitem);
    sb_globals.percentiles[i] = atof(val->data);
    i++;
  }

  sb_globals.histogram = sb_get_value_flag("histogram");
  if (sb_globals.npercentiles == 0 && sb_globals.histogram != 0)
  {
    log_text(LOG_FATAL, "--histogram cannot be used with --percentile=NULL");
    return 1;
  }

  if (sb_histogram_init(&sb_latency_histogram, OPER_LOG_GRANULARITY,
                        OPER_LOG_MIN_VALUE, OPER_LOG_MAX_VALUE))
    return 1;

  return 0;
}


/* Uninitialize operations messages handler */

int oper_handler_done(void)
{
  sb_histogram_done(&sb_latency_histogram);

  return 0;
}

char *create_pct_string(double* percentiles, double* results, size_t npercentiles, char* format_string, int max_str_length){
  char *res = malloc(sizeof(char));
  *res = '\0';
  for(size_t i = 0; i < npercentiles; i++){
    char *to_append = malloc(max_str_length * sizeof(char));
        sprintf(to_append, format_string, *(percentiles + i), SEC2MS(*(results + i)));
    char *buf = malloc((strlen(res) + strlen(to_append)) * sizeof(char) + 1);
    *buf = '\0';
    buf = strcat(buf, res);
    free(res);
    res = strcat(buf, to_append);
    free(to_append);
  }
  return res;

}

char *create_pct_string_intermediate(double* percentiles, double* results, size_t npercentiles){
  char *format_string = "lat (ms,%5.2f%%): %4.2f ";
  return create_pct_string(percentiles, results, npercentiles, format_string, strlen(format_string) + 6 + 10 + 1); //6 is the maximum number of chars for 100.00 to be formatted, 10 is the maximum number of chars for an assumed upper bound of 1000000.00ms latency.
}

char *create_pct_string_cumulative(double* percentiles, double* results, size_t npercentiles){
  char *format_string = "         %5.2fth percentile:%25.2f\n";
  return create_pct_string(percentiles, results, npercentiles, format_string, strlen(format_string) + 6 + 25 + 1); //6 is the maximum number of chars for 100.00 to be formatted
}
