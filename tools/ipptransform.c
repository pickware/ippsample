/*
 * Utility for converting PDF and JPEG files to raster data or HP PCL.
 *
 * Copyright © 2016-2019 by the IEEE-ISTO Printer Working Group.
 * Copyright © 2016-2019 by Apple Inc.
 *
 * Licensed under Apache License v2.0.  See the file "LICENSE" for more
 * information.
 */

#define HAVE_COREGRAPHICS 1

#include <cups/cups.h>
#include <cups/raster.h>
#include <cups/array-private.h>
#include <cups/string-private.h>
#include <cups/thread-private.h>

#ifdef HAVE_COREGRAPHICS
#  include <CoreGraphics/CoreGraphics.h>
#  include <ImageIO/ImageIO.h>

extern void CGContextSetCTM(CGContextRef c, CGAffineTransform m);
#elif defined(HAVE_MUPDF)
#  include <mupdf/fitz.h>
static inline fz_matrix make_matrix(float a, float b, float c, float d, float e, float f) {
  fz_matrix ret = { a, b, c, d, e, f };
  return (ret);
}
#endif /* HAVE_COREGRAPHICS */

#include "dither.h"


/*
 * Constants...
 */

#define XFORM_MAX_RASTER	16777216

#define XFORM_RED_MASK		0x000000ff
#define XFORM_GREEN_MASK	0x0000ff00
#define XFORM_BLUE_MASK		0x00ff0000
#define XFORM_RGB_MASK		(XFORM_RED_MASK | XFORM_GREEN_MASK |  XFORM_BLUE_MASK)
#define XFORM_BG_MASK		(XFORM_BLUE_MASK | XFORM_GREEN_MASK)
#define XFORM_RG_MASK		(XFORM_RED_MASK | XFORM_GREEN_MASK)


/*
 * Local types...
 */

typedef void * (*renderer_make_renderer_cb_t)();
typedef void (*renderer_deallocate_cb_t)(void *);
typedef bool (*renderer_open_document_cb_t)(CFURLRef, void *);
typedef int  (*renderer_get_page_count_cb_t)(void *);
typedef bool (*renderer_load_page_cb_t)(const int, void *);
typedef CGRect (*renderer_get_page_rect_cb_t)(void *);
typedef CGAffineTransform  (*renderer_get_page_transform_cb_t)(void *);
typedef bool (*renderer_render_cb_t)(CGContextRef, void *);

extern struct renderer {
    renderer_make_renderer_cb_t makeRenderer;
    renderer_deallocate_cb_t deallocate;
    renderer_open_document_cb_t openDocument;
    renderer_get_page_count_cb_t getPageCount;
    renderer_load_page_cb_t loadPage;
    renderer_get_page_rect_cb_t getPageRect;
    renderer_get_page_transform_cb_t getPageTransform;
    renderer_render_cb_t render;
};

typedef ssize_t (*xform_write_cb_t)(void *, const unsigned char *, size_t);

typedef struct xform_raster_s xform_raster_t;

struct xform_raster_s
{
  const char		*format;	/* Output format */
  int			num_options;	/* Number of job options */
  cups_option_t		*options;	/* Job options */
  unsigned		copies;		/* Number of copies */
  cups_page_header2_t	header;		/* Page header */
  cups_page_header2_t	back_header;	/* Page header for back side */
  int			borderless;	/* Borderless media? */
  unsigned char		*band_buffer;	/* Band buffer */
  unsigned		band_height;	/* Band height */
  unsigned		band_bpp;	/* Bytes per pixel in band */

  /* Set by start_job callback */
  cups_raster_t		*ras;		/* Raster stream */

  /* Set by start_page callback */
  unsigned		left, top, right, bottom;
					/* Image (print) box with origin at top left */
  unsigned		out_blanks;	/* Blank lines */
  size_t		out_length;	/* Output buffer size */
  unsigned char		*out_buffer;	/* Output (bit) buffer */
  unsigned char		*comp_buffer;	/* Compression buffer */

  unsigned char		dither[64][64];	/* Dither array */

  /* Callbacks */
  void			(*end_job)(xform_raster_t *, xform_write_cb_t, void *);
  void			(*end_page)(xform_raster_t *, unsigned, xform_write_cb_t, void *);
  void			(*start_job)(xform_raster_t *, xform_write_cb_t, void *);
  void			(*start_page)(xform_raster_t *, unsigned, xform_write_cb_t, void *);
  void			(*write_line)(xform_raster_t *, unsigned, const unsigned char *, xform_write_cb_t, void *);
};


/*
 * Local globals...
 */

static int	Verbosity = 0;		/* Log level */


/*
 * Local functions...
 */

#ifdef HAVE_MUPDF
static void	invert_gray(unsigned char *row, size_t num_pixels);
#endif /* HAVE_MUPDF */
static int	load_env_options(cups_option_t **options);
static void	*monitor_ipp(const char *device_uri);
#ifdef HAVE_COREGRAPHICS
static void	pack_rgba(unsigned char *row, size_t num_pixels);
static void	pack_rgba16(unsigned char *row, size_t num_pixels);
#endif /* HAVE_COREGRAPHICS */
static void	pcl_end_job(xform_raster_t *ras, xform_write_cb_t cb, void *ctx);
static void	pcl_end_page(xform_raster_t *ras, unsigned page, xform_write_cb_t cb, void *ctx);
static void	pcl_init(xform_raster_t *ras);
static void	pcl_printf(xform_write_cb_t cb, void *ctx, const char *format, ...) _CUPS_FORMAT(3, 4);
static void	pcl_start_job(xform_raster_t *ras, xform_write_cb_t cb, void *ctx);
static void	pcl_start_page(xform_raster_t *ras, unsigned page, xform_write_cb_t cb, void *ctx);
static void	pcl_write_line(xform_raster_t *ras, unsigned y, const unsigned char *line, xform_write_cb_t cb, void *ctx);
static void	raster_end_job(xform_raster_t *ras, xform_write_cb_t cb, void *ctx);
static void	raster_end_page(xform_raster_t *ras, unsigned page, xform_write_cb_t cb, void *ctx);
static void	raster_init(xform_raster_t *ras);
static void	raster_start_job(xform_raster_t *ras, xform_write_cb_t cb, void *ctx);
static void	raster_start_page(xform_raster_t *ras, unsigned page, xform_write_cb_t cb, void *ctx);
static void	raster_write_line(xform_raster_t *ras, unsigned y, const unsigned char *line, xform_write_cb_t cb, void *ctx);
static void	usage(int status) _CUPS_NORETURN;
static ssize_t	write_fd(int *fd, const unsigned char *buffer, size_t bytes);
int    xform_document(const char *filename, const char *informat, const char *outformat, const char *resolutions, const char *sheet_back, const char *types, int num_options, cups_option_t *options, xform_write_cb_t cb, struct renderer renderer, void *ctx);
static int	xform_setup(xform_raster_t *ras, const char *outformat, const char *resolutions, const char *types, const char *sheet_back, int color, unsigned pages, int num_options, cups_option_t *options);


/*
 * 'main()' - Main entry for transform utility.
 */

int					/* O - Exit status */
main(int  argc,				/* I - Number of command-line args */
     char *argv[])			/* I - Command-line arguments */
{
  int		i;			/* Looping var */
  const char	*filename = NULL,	/* File to transform */
		*content_type,		/* Source content type */
		*device_uri,		/* Destination URI */
		*output_type,		/* Destination content type */
		*resolutions,		/* pwg-raster-document-resolution-supported */
		*sheet_back,		/* pwg-raster-document-sheet-back */
		*types,			/* pwg-raster-document-type-supported */
		*opt;			/* Option character */
  int		num_options;		/* Number of options */
  cups_option_t	*options;		/* Options */
  int		fd = 1;			/* Output file/socket */
  http_t	*http = NULL;		/* Output HTTP connection */
  void		*write_ptr = &fd;	/* Pointer to file/socket/HTTP connection */
  char		resource[1024];		/* URI resource path */
  xform_write_cb_t write_cb = (xform_write_cb_t)write_fd;
					/* Write callback */
  int		status = 0;		/* Exit status */
  _cups_thread_t monitor = 0;		/* Monitoring thread ID */


 /*
  * Process the command-line...
  */

  num_options  = load_env_options(&options);
  content_type = getenv("CONTENT_TYPE");
  device_uri   = getenv("DEVICE_URI");
  output_type  = getenv("OUTPUT_TYPE");
  resolutions  = getenv("IPP_PWG_RASTER_DOCUMENT_RESOLUTION_SUPPORTED");
  sheet_back   = getenv("IPP_PWG_RASTER_DOCUMENT_SHEET_BACK");
  types        = getenv("IPP_PWG_RASTER_DOCUMENT_TYPE_SUPPORTED");

  if ((opt = getenv("SERVER_LOGLEVEL")) != NULL)
  {
    if (!strcmp(opt, "debug"))
      Verbosity = 2;
    else if (!strcmp(opt, "info"))
      Verbosity = 1;
  }

  for (i = 1; i < argc; i ++)
  {
    if (!strncmp(argv[i], "--", 2))
    {
      if (!strcmp(argv[i], "--help"))
      {
        usage(0);
      }
      else if (!strcmp(argv[i], "--version"))
      {
        puts(CUPS_SVERSION);
      }
      else
      {
	fprintf(stderr, "ERROR: Unknown option '%s'.\n", argv[i]);
	usage(1);
      }
    }
    else if (argv[i][0] == '-')
    {
      for (opt = argv[i] + 1; *opt; opt ++)
      {
        switch (*opt)
	{
	  case 'd' :
	      i ++;
	      if (i >= argc)
	      {
	        fputs("ERROR: Missing argument after '-d'.\n", stderr);
	        usage(1);
	      }

	      device_uri = argv[i];
	      break;

	  case 'f' :
	      i ++;
	      if (i >= argc)
	      {
	        fputs("ERROR: Missing argument after '-f'.\n", stderr);
	        usage(1);
	      }

	      stdout = freopen(argv[i], "w", stdout);
	      break;

	  case 'i' :
	      i ++;
	      if (i >= argc)
	      {
	        fputs("ERROR: Missing argument after '-i'.\n", stderr);
	        usage(1);
	      }

	      content_type = argv[i];
	      break;

	  case 'm' :
	      i ++;
	      if (i >= argc)
	      {
	        fputs("ERROR: Missing argument after '-m'.\n", stderr);
	        usage(1);
	      }

	      output_type = argv[i];
	      break;

	  case 'o' :
	      i ++;
	      if (i >= argc)
	      {
	        fputs("ERROR: Missing argument after '-o'.\n", stderr);
	        usage(1);
	      }

	      num_options = cupsParseOptions(argv[i], num_options, &options);
	      break;

	  case 'r' : /* pwg-raster-document-resolution-supported values */
	      i ++;
	      if (i >= argc)
	      {
	        fputs("ERROR: Missing argument after '-r'.\n", stderr);
	        usage(1);
	      }

	      resolutions = argv[i];
	      break;

	  case 's' : /* pwg-raster-document-sheet-back value */
	      i ++;
	      if (i >= argc)
	      {
	        fputs("ERROR: Missing argument after '-s'.\n", stderr);
	        usage(1);
	      }

	      sheet_back = argv[i];
	      break;

	  case 't' : /* pwg-raster-document-type-supported values */
	      i ++;
	      if (i >= argc)
	      {
	        fputs("ERROR: Missing argument after '-t'.\n", stderr);
	        usage(1);
	      }

	      types = argv[i];
	      break;

	  case 'v' : /* Be verbose... */
	      Verbosity ++;
	      break;

	  default :
	      fprintf(stderr, "ERROR: Unknown option '-%c'.\n", *opt);
	      usage(1);
	      break;
	}
      }
    }
    else if (!filename)
      filename = argv[i];
    else
    {
      fprintf(stderr, "ERROR: Unknown argument '%s'.\n", argv[i]);
      usage(1);
    }
  }

 /*
  * Check that we have everything we need...
  */

  if (!filename)
    usage(1);

  if (!content_type)
  {
    if ((opt = strrchr(filename, '.')) != NULL)
    {
      if (!strcmp(opt, ".pdf"))
        content_type = "application/pdf";
      else if (!strcmp(opt, ".jpg") || !strcmp(opt, ".jpeg"))
        content_type = "image/jpeg";
    }
  }

  if (!content_type)
  {
    fprintf(stderr, "ERROR: Unknown format for \"%s\", please specify with '-i' option.\n", filename);
    usage(1);
  }
  else if (strcmp(content_type, "application/pdf") && strcmp(content_type, "image/jpeg"))
  {
    fprintf(stderr, "ERROR: Unsupported format \"%s\" for \"%s\".\n", content_type, filename);
    usage(1);
  }

  if (!output_type)
  {
    fputs("ERROR: Unknown output format, please specify with '-m' option.\n", stderr);
    usage(1);
  }
  else if (strcmp(output_type, "application/vnd.hp-pcl") && strcmp(output_type, "image/pwg-raster") && strcmp(output_type, "image/urf"))
  {
    fprintf(stderr, "ERROR: Unsupported output format \"%s\".\n", output_type);
    usage(1);
  }

  if (!resolutions)
    resolutions = "300dpi";
  if (!sheet_back)
    sheet_back = "normal";
  if (!types)
    types = "sgray_8";

 /*
  * If the device URI is specified, open the connection...
  */

  if (device_uri)
  {
    char	scheme[32],		/* URI scheme */
		userpass[256],		/* URI user:pass */
		host[256],		/* URI host */
		service[32];		/* Service port */
    int		port;			/* URI port number */
    http_addrlist_t *list;		/* Address list for socket */

    if (httpSeparateURI(HTTP_URI_CODING_ALL, device_uri, scheme, sizeof(scheme), userpass, sizeof(userpass), host, sizeof(host), &port, resource, sizeof(resource)) < HTTP_URI_STATUS_OK)
    {
      fprintf(stderr, "ERROR: Invalid device URI \"%s\".\n", device_uri);
      usage(1);
    }

    if (strcmp(scheme, "socket") && strcmp(scheme, "ipp") && strcmp(scheme, "ipps"))
    {
      fprintf(stderr, "ERROR: Unsupported device URI scheme \"%s\".\n", scheme);
      usage(1);
    }

    snprintf(service, sizeof(service), "%d", port);
    if ((list = httpAddrGetList(host, AF_UNSPEC, service)) == NULL)
    {
      fprintf(stderr, "ERROR: Unable to lookup device URI host \"%s\": %s\n", host, cupsLastErrorString());
      return (1);
    }

    if (!strcmp(scheme, "socket"))
    {
     /*
      * AppSocket connection...
      */

      if (!httpAddrConnect2(list, &fd, 30000, NULL))
      {
	fprintf(stderr, "ERROR: Unable to connect to \"%s\" on port %d: %s\n", host, port, cupsLastErrorString());
	return (1);
      }
    }
    else
    {
      http_encryption_t encryption;	/* Encryption mode */
      ipp_t		*request,	/* IPP request */
			*response;	/* IPP response */
      ipp_attribute_t	*attr;		/* operations-supported */
      int		create_job = 0;	/* Support for Create-Job/Send-Document? */
      int		gzip;		/* gzip compression supported? */
      const char	*job_name;	/* Title of job */
      const char	*media;		/* Value of "media" option */
      const char	*sides;		/* Value of "sides" option */
      static const char * const pattrs[] =
      {					/* requested-attributes */
        "compression-supported",
        "operations-supported"
      };

     /*
      * Connect to the IPP/IPPS printer...
      */

      if (port == 443 || !strcmp(scheme, "ipps"))
        encryption = HTTP_ENCRYPTION_ALWAYS;
      else
        encryption = HTTP_ENCRYPTION_IF_REQUESTED;

      if ((http = httpConnect2(host, port, list, AF_UNSPEC, encryption, 1, 30000, NULL)) == NULL)
      {
	fprintf(stderr, "ERROR: Unable to connect to \"%s\" on port %d: %s\n", host, port, cupsLastErrorString());
	return (1);
      }

     /*
      * See if it supports Create-Job + Send-Document...
      */

      request = ippNewRequest(IPP_OP_GET_PRINTER_ATTRIBUTES);
      ippAddString(request, IPP_TAG_OPERATION, IPP_TAG_URI, "printer-uri", NULL, device_uri);
      ippAddString(request, IPP_TAG_OPERATION, IPP_TAG_NAME, "requesting-user-name", NULL, cupsUser());
      ippAddStrings(request, IPP_TAG_OPERATION, IPP_TAG_KEYWORD, "requested-attributes", (int)(sizeof(pattrs) / sizeof(pattrs[0])), NULL, pattrs);

      response = cupsDoRequest(http, request, resource);
      if (cupsLastError() > IPP_STATUS_OK_EVENTS_COMPLETE)
      {
        fprintf(stderr, "ERROR: Unable to get printer capabilities: %s\n", cupsLastErrorString());
	return (1);
      }

      if ((attr = ippFindAttribute(response, "operations-supported", IPP_TAG_ENUM)) == NULL)
      {
        fputs("ERROR: Unable to get list of supported operations from printer.\n", stderr);
	return (1);
      }

      create_job = ippContainsInteger(attr, IPP_OP_CREATE_JOB) && ippContainsInteger(attr, IPP_OP_SEND_DOCUMENT);
      gzip       = ippContainsString(ippFindAttribute(response, "compression-supported", IPP_TAG_KEYWORD), "gzip");

      ippDelete(response);

     /*
      * Create the job and start printing...
      */

      if ((job_name = getenv("IPP_JOB_NAME")) == NULL)
      {
	if ((job_name = strrchr(filename, '/')) != NULL)
	  job_name ++;
	else
	  job_name = filename;
      }

      if (create_job)
      {
        int		job_id = 0;	/* Job ID */

        request = ippNewRequest(IPP_OP_CREATE_JOB);
	ippAddString(request, IPP_TAG_OPERATION, IPP_TAG_URI, "printer-uri", NULL, device_uri);
	ippAddString(request, IPP_TAG_OPERATION, IPP_TAG_NAME, "requesting-user-name", NULL, cupsUser());
	ippAddString(request, IPP_TAG_OPERATION, IPP_TAG_NAME, "job-name", NULL, job_name);

        response = cupsDoRequest(http, request, resource);
        if ((attr = ippFindAttribute(response, "job-id", IPP_TAG_INTEGER)) != NULL)
	  job_id = ippGetInteger(attr, 0);
        ippDelete(response);

	if (cupsLastError() > IPP_STATUS_OK_EVENTS_COMPLETE)
	{
	  fprintf(stderr, "ERROR: Unable to create print job: %s\n", cupsLastErrorString());
	  return (1);
	}
	else if (job_id <= 0)
	{
          fputs("ERROR: No job-id for created print job.\n", stderr);
	  return (1);
	}

        request = ippNewRequest(IPP_OP_SEND_DOCUMENT);
	ippAddString(request, IPP_TAG_OPERATION, IPP_TAG_URI, "printer-uri", NULL, device_uri);
	ippAddInteger(request, IPP_TAG_OPERATION, IPP_TAG_INTEGER, "job-id", job_id);
	ippAddString(request, IPP_TAG_OPERATION, IPP_TAG_NAME, "requesting-user-name", NULL, cupsUser());
	ippAddString(request, IPP_TAG_OPERATION, IPP_TAG_MIMETYPE, "document-format", NULL, output_type);
	if (gzip)
	  ippAddString(request, IPP_TAG_OPERATION, IPP_TAG_KEYWORD, "compression", NULL, "gzip");
        ippAddBoolean(request, IPP_TAG_OPERATION, "last-document", 1);
      }
      else
      {
        request = ippNewRequest(IPP_OP_PRINT_JOB);
	ippAddString(request, IPP_TAG_OPERATION, IPP_TAG_URI, "printer-uri", NULL, device_uri);
	ippAddString(request, IPP_TAG_OPERATION, IPP_TAG_NAME, "requesting-user-name", NULL, cupsUser());
	ippAddString(request, IPP_TAG_OPERATION, IPP_TAG_MIMETYPE, "document-format", NULL, output_type);
	if (gzip)
	  ippAddString(request, IPP_TAG_OPERATION, IPP_TAG_KEYWORD, "compression", NULL, "gzip");
      }

      if ((media = cupsGetOption("media", num_options, options)) != NULL)
        ippAddString(request, IPP_TAG_JOB, IPP_TAG_KEYWORD, "media", NULL, media);

      if ((sides = cupsGetOption("sides", num_options, options)) != NULL)
        ippAddString(request, IPP_TAG_JOB, IPP_TAG_KEYWORD, "sides", NULL, sides);

      if (cupsSendRequest(http, request, resource, 0) != HTTP_STATUS_CONTINUE)
      {
        fprintf(stderr, "ERROR: Unable to send print data: %s\n", cupsLastErrorString());
	return (1);
      }

      ippDelete(request);

      if (gzip)
        httpSetField(http, HTTP_FIELD_CONTENT_ENCODING, "gzip");

      write_cb  = (xform_write_cb_t)httpWrite2;
      write_ptr = http;

      monitor = _cupsThreadCreate((_cups_thread_func_t)monitor_ipp, (void *)device_uri);
    }

    httpAddrFreeList(list);
  }

 /*
  * Do transform...
  */

  // only used for command line
  // status = xform_document(filename, content_type, output_type, resolutions, sheet_back, types, num_options, options, write_cb, write_ptr);

  if (http)
  {
    ippDelete(cupsGetResponse(http, resource));

    if (cupsLastError() > IPP_STATUS_OK_EVENTS_COMPLETE)
    {
      fprintf(stderr, "ERROR: Unable to send print data: %s\n", cupsLastErrorString());
      status = 1;
    }

    httpClose(http);
  }
  else if (fd != 1)
    close(fd);

  if (monitor)
    _cupsThreadCancel(monitor);

  return (status);
}


/*
 * 'invert_gray()' - Invert grayscale to black.
 */

#ifdef HAVE_MUPDF
static void
invert_gray(unsigned char *row,		/* I - Current row */
            size_t        num_pixels)	/* I - Number of pixels */
{
  unsigned	*ptr;			/* Pointer to 32-bits worth of pixels */


  ptr = (unsigned *)row;
  while (num_pixels > 3)
  {
    *ptr = ~*ptr;
    ptr ++;
    num_pixels -= 4;
  }

  if (num_pixels > 0)
  {
    row = (unsigned char *)ptr;
    while (num_pixels > 0)
    {
      *row = ~*row;
      row ++;
      num_pixels --;
    }
  }
}
#endif /* HAVE_MUPDF */


/*
 * 'load_env_options()' - Load options from the environment.
 */

extern char **environ;

static int				/* O - Number of options */
load_env_options(
    cups_option_t **options)		/* I - Options */
{
  int	i;				/* Looping var */
  char	name[256],			/* Option name */
	*nameptr,			/* Pointer into name */
	*envptr;			/* Pointer into environment variable */
  int	num_options = 0;		/* Number of options */


  *options = NULL;

 /*
  * Load all of the IPP_xxx environment variables as options...
  */

  for (i = 0; environ[i]; i ++)
  {
    envptr = environ[i];

    if (strncmp(envptr, "IPP_", 4))
      continue;

    for (nameptr = name, envptr += 4; *envptr && *envptr != '='; envptr ++)
    {
      if (nameptr > (name + sizeof(name) - 1))
        continue;

      if (*envptr == '_')
        *nameptr++ = '-';
      else
        *nameptr++ = (char)_cups_tolower(*envptr);
    }

    *nameptr = '\0';
    if (*envptr == '=')
      envptr ++;

    num_options = cupsAddOption(name, envptr, num_options, options);
  }

  return (num_options);
}


/*
 * 'monitor_ipp()' - Monitor IPP printer status.
 */

static void *				/* O - Thread exit status */
monitor_ipp(const char *device_uri)	/* I - Device URI */
{
  int		i;			/* Looping var */
  http_t	*http;			/* HTTP connection */
  ipp_t		*request,		/* IPP request */
		*response;		/* IPP response */
  ipp_attribute_t *attr;		/* IPP response attribute */
  char		scheme[32],		/* URI scheme */
		userpass[256],		/* URI user:pass */
		host[256],		/* URI host */
		resource[1024];		/* URI resource */
  int		port;			/* URI port number */
  http_encryption_t encryption;		/* Encryption to use */
  int		delay = 1,		/* Current delay */
		next_delay,		/* Next delay */
		prev_delay = 0;		/* Previous delay */
  char		pvalues[10][1024];	/* Current printer attribute values */
  static const char * const pattrs[10] =/* Printer attributes we need */
  {
    "marker-colors",
    "marker-levels",
    "marker-low-levels",
    "marker-high-levels",
    "marker-names",
    "marker-types",
    "printer-alert",
    "printer-state-reasons",
    "printer-supply",
    "printer-supply-description"
  };


  httpSeparateURI(HTTP_URI_CODING_ALL, device_uri, scheme, sizeof(scheme), userpass, sizeof(userpass), host, sizeof(host), &port, resource, sizeof(resource));

  if (port == 443 || !strcmp(scheme, "ipps"))
    encryption = HTTP_ENCRYPTION_ALWAYS;
  else
    encryption = HTTP_ENCRYPTION_IF_REQUESTED;

  while ((http = httpConnect2(host, port, NULL, AF_UNSPEC, encryption, 1, 30000, NULL)) == NULL)
  {
    fprintf(stderr, "ERROR: Unable to connect to \"%s\" on port %d: %s\n", host, port, cupsLastErrorString());
    sleep(30);
  }

 /*
  * Report printer state changes until we are canceled...
  */

  for (;;)
  {
   /*
    * Poll for the current state...
    */

    request = ippNewRequest(IPP_OP_GET_PRINTER_ATTRIBUTES);
    ippAddString(request, IPP_TAG_OPERATION, IPP_TAG_URI, "printer-uri", NULL, device_uri);
    ippAddString(request, IPP_TAG_OPERATION, IPP_TAG_NAME, "requesting-user-name", NULL, cupsUser());
    ippAddStrings(request, IPP_TAG_OPERATION, IPP_TAG_KEYWORD, "requested-attributes", (int)(sizeof(pattrs) / sizeof(pattrs[0])), NULL, pattrs);

    response = cupsDoRequest(http, request, resource);

   /*
    * Report any differences...
    */

    for (attr = ippFirstAttribute(response); attr; attr = ippNextAttribute(response))
    {
      const char *name = ippGetName(attr);
      char	value[1024];		/* Name and value */


      if (!name)
        continue;

      for (i = 0; i < (int)(sizeof(pattrs) / sizeof(pattrs[0])); i ++)
        if (!strcmp(name, pattrs[i]))
	  break;

      if (i >= (int)(sizeof(pattrs) / sizeof(pattrs[0])))
        continue;

      ippAttributeString(attr, value, sizeof(value));

      if (strcmp(value, pvalues[i]))
      {
        if (!strcmp(name, "printer-state-reasons"))
	  fprintf(stderr, "STATE: %s\n", value);
	else
	  fprintf(stderr, "ATTR: %s='%s'\n", name, value);

        strlcpy(pvalues[i], value, sizeof(pvalues[i]));
      }
    }

    ippDelete(response);

   /*
    * Sleep until the next update...
    */

    sleep((unsigned)delay);

    next_delay = (delay + prev_delay) % 12;
    prev_delay = next_delay < delay ? 0 : delay;
    delay      = next_delay;
  }

  return (NULL);
}


#ifdef HAVE_COREGRAPHICS
/*
 * 'pack_rgba()' - Pack RGBX scanlines into RGB scanlines.
 *
 * This routine is suitable only for 8 bit RGBX data packed into RGB bytes.
 */

static void
pack_rgba(unsigned char *row,		/* I - Row of pixels to pack */
	  size_t        num_pixels)	/* I - Number of pixels in row */
{
  size_t	num_quads = num_pixels / 4;
					/* Number of 4 byte samples to pack */
  size_t	leftover_pixels = num_pixels & 3;
					/* Number of pixels remaining */
  unsigned	*quad_row = (unsigned *)row;
					/* 32-bit pixel pointer */
  unsigned	*dest = quad_row;	/* Destination pointer */
  unsigned char *src_byte;		/* Remaining source bytes */
  unsigned char *dest_byte;		/* Remaining destination bytes */


 /*
  * Copy all of the groups of 4 pixels we can...
  */

  while (num_quads > 0)
  {
    *dest++ = (quad_row[0] & XFORM_RGB_MASK) | (quad_row[1] << 24);
    *dest++ = ((quad_row[1] & XFORM_BG_MASK) >> 8) |
              ((quad_row[2] & XFORM_RG_MASK) << 16);
    *dest++ = ((quad_row[2] & XFORM_BLUE_MASK) >> 16) | (quad_row[3] << 8);
    quad_row += 4;
    num_quads --;
  }

 /*
  * Then handle the leftover pixels...
  */

  src_byte  = (unsigned char *)quad_row;
  dest_byte = (unsigned char *)dest;

  while (leftover_pixels > 0)
  {
    *dest_byte++ = *src_byte++;
    *dest_byte++ = *src_byte++;
    *dest_byte++ = *src_byte++;
    src_byte ++;
    leftover_pixels --;
  }
}


/*
 * 'pack_rgba16()' - Pack 16 bit per component RGBX scanlines into RGB scanlines.
 *
 * This routine is suitable only for 16 bit RGBX data packed into RGB bytes.
 */

static void
pack_rgba16(unsigned char *row,		/* I - Row of pixels to pack */
	    size_t        num_pixels)	/* I - Number of pixels in row */
{
  const unsigned	*from = (unsigned *)row;
					/* 32 bits from row */
  unsigned		*dest = (unsigned *)row;
					/* Destination pointer */


  while (num_pixels > 1)
  {
    *dest++ = from[0];
    *dest++ = (from[1] & 0x0000ffff) | ((from[2] & 0x0000ffff) << 16);
    *dest++ = ((from[2] & 0xffff0000) >> 16) | ((from[3] & 0x0000ffff) << 16);
    from += 4;
    num_pixels -= 2;
  }

  if (num_pixels)
  {
    *dest++ = *from++;
    *dest++ = *from++;
  }
}
#endif /* HAVE_COREGRAPHICS */


/*
 * 'pcl_end_job()' - End a PCL "job".
 */

static void
pcl_end_job(xform_raster_t   *ras,	/* I - Raster information */
            xform_write_cb_t cb,	/* I - Write callback */
            void             *ctx)	/* I - Write context */
{
  (void)ras;

 /*
  * Send a PCL reset sequence.
  */

  (*cb)(ctx, (const unsigned char *)"\033E", 2);
}


/*
 * 'pcl_end_page()' - End of PCL page.
 */

static void
pcl_end_page(xform_raster_t   *ras,	/* I - Raster information */
	     unsigned         page,	/* I - Current page */
             xform_write_cb_t cb,	/* I - Write callback */
             void             *ctx)	/* I - Write context */
{
 /*
  * End graphics...
  */

  (*cb)(ctx, (const unsigned char *)"\033*r0B", 5);

 /*
  * Formfeed as needed...
  */

  if (!(ras->header.Duplex && (page & 1)))
    (*cb)(ctx, (const unsigned char *)"\014", 1);

 /*
  * Free the output buffer...
  */

  free(ras->out_buffer);
  ras->out_buffer = NULL;
}


/*
 * 'pcl_init()' - Initialize callbacks for PCL output.
 */

static void
pcl_init(xform_raster_t *ras)		/* I - Raster information */
{
  ras->end_job    = pcl_end_job;
  ras->end_page   = pcl_end_page;
  ras->start_job  = pcl_start_job;
  ras->start_page = pcl_start_page;
  ras->write_line = pcl_write_line;
}


/*
 * 'pcl_printf()' - Write a formatted string.
 */

static void
pcl_printf(xform_write_cb_t cb,		/* I - Write callback */
           void             *ctx,	/* I - Write context */
	   const char       *format,	/* I - Printf-style format string */
	   ...)				/* I - Additional arguments as needed */
{
  va_list	ap;			/* Argument pointer */
  char		buffer[8192];		/* Buffer */


  va_start(ap, format);
  vsnprintf(buffer, sizeof(buffer), format, ap);
  va_end(ap);

  (*cb)(ctx, (const unsigned char *)buffer, strlen(buffer));
}


/*
 * 'pcl_start_job()' - Start a PCL "job".
 */

static void
pcl_start_job(xform_raster_t   *ras,	/* I - Raster information */
              xform_write_cb_t cb,	/* I - Write callback */
              void             *ctx)	/* I - Write context */
{
  (void)ras;

 /*
  * Send a PCL reset sequence.
  */

  (*cb)(ctx, (const unsigned char *)"\033E", 2);
}


/*
 * 'pcl_start_page()' - Start a PCL page.
 */

static void
pcl_start_page(xform_raster_t   *ras,	/* I - Raster information */
               unsigned         page,	/* I - Current page */
               xform_write_cb_t cb,	/* I - Write callback */
               void             *ctx)	/* I - Write context */
{
 /*
  * Setup margins to be 1/6" top and bottom and 1/4" or .135" on the
  * left and right.
  */

  ras->top    = ras->header.HWResolution[1] / 6;
  ras->bottom = ras->header.cupsHeight - ras->header.HWResolution[1] / 6;

  if (ras->header.PageSize[1] == 842)
  {
   /* A4 gets special side margins to expose an 8" print area */
    ras->left  = (ras->header.cupsWidth - 8 * ras->header.HWResolution[0]) / 2;
    ras->right = ras->left + 8 * ras->header.HWResolution[0];
  }
  else
  {
   /* All other sizes get 1/4" margins */
    ras->left  = ras->header.HWResolution[0] / 4;
    ras->right = ras->header.cupsWidth - ras->header.HWResolution[0] / 4;
  }

  if (!ras->header.Duplex || (page & 1))
  {
   /*
    * Set the media size...
    */

    pcl_printf(cb, ctx, "\033&l12D\033&k12H");
					/* Set 12 LPI, 10 CPI */
    pcl_printf(cb, ctx, "\033&l0O");	/* Set portrait orientation */

    switch (ras->header.PageSize[1])
    {
      case 540 : /* Monarch Envelope */
          pcl_printf(cb, ctx, "\033&l80A");
	  break;

      case 595 : /* A5 */
          pcl_printf(cb, ctx, "\033&l25A");
	  break;

      case 624 : /* DL Envelope */
          pcl_printf(cb, ctx, "\033&l90A");
	  break;

      case 649 : /* C5 Envelope */
          pcl_printf(cb, ctx, "\033&l91A");
	  break;

      case 684 : /* COM-10 Envelope */
          pcl_printf(cb, ctx, "\033&l81A");
	  break;

      case 709 : /* B5 Envelope */
          pcl_printf(cb, ctx, "\033&l100A");
	  break;

      case 756 : /* Executive */
          pcl_printf(cb, ctx, "\033&l1A");
	  break;

      case 792 : /* Letter */
          pcl_printf(cb, ctx, "\033&l2A");
	  break;

      case 842 : /* A4 */
          pcl_printf(cb, ctx, "\033&l26A");
	  break;

      case 1008 : /* Legal */
          pcl_printf(cb, ctx, "\033&l3A");
	  break;

      case 1191 : /* A3 */
          pcl_printf(cb, ctx, "\033&l27A");
	  break;

      case 1224 : /* Tabloid */
          pcl_printf(cb, ctx, "\033&l6A");
	  break;
    }

   /*
    * Set top margin and turn off perforation skip...
    */

    pcl_printf(cb, ctx, "\033&l%uE\033&l0L", 12 * ras->top / ras->header.HWResolution[1]);

    if (ras->header.Duplex)
    {
      int mode = ras->header.Duplex ? 1 + ras->header.Tumble != 0 : 0;

      pcl_printf(cb, ctx, "\033&l%dS", mode);
					/* Set duplex mode */
    }
  }
  else if (ras->header.Duplex)
    pcl_printf(cb, ctx, "\033&a2G");	/* Print on back side */

 /*
  * Set graphics mode...
  */

  pcl_printf(cb, ctx, "\033*t%uR", ras->header.HWResolution[0]);
					/* Set resolution */
  pcl_printf(cb, ctx, "\033*r%uS", ras->right - ras->left);
					/* Set width */
  pcl_printf(cb, ctx, "\033*r%uT", ras->bottom - ras->top);
					/* Set height */
  pcl_printf(cb, ctx, "\033&a0H\033&a%uV", 720 * ras->top / ras->header.HWResolution[1]);
					/* Set position */

  pcl_printf(cb, ctx, "\033*b2M");	/* Use PackBits compression */
  pcl_printf(cb, ctx, "\033*r1A");	/* Start graphics */

 /*
  * Allocate the output buffer...
  */

  ras->out_blanks  = 0;
  ras->out_length  = (ras->right - ras->left + 7) / 8;
  ras->out_buffer  = malloc(ras->out_length);
  ras->comp_buffer = malloc(2 * ras->out_length + 2);
}


/*
 * 'pcl_write_line()' - Write a line of raster data.
 */

static void
pcl_write_line(
    xform_raster_t      *ras,		/* I - Raster information */
    unsigned            y,		/* I - Line number */
    const unsigned char *line,		/* I - Pixels on line */
    xform_write_cb_t    cb,		/* I - Write callback */
    void                *ctx)		/* I - Write context */
{
  unsigned	x;			/* Column number */
  unsigned char	bit,			/* Current bit */
		byte,			/* Current byte */
		*outptr,		/* Pointer into output buffer */
		*outend,		/* End of output buffer */
		*start,			/* Start of sequence */
		*compptr;		/* Pointer into compression buffer */
  unsigned	count;			/* Count of bytes for output */
  const unsigned char	*ditherline;	/* Pointer into dither table */


  if (line[0] == 255 && !memcmp(line, line + 1, ras->right - ras->left - 1))
  {
   /*
    * Skip blank line...
    */

    ras->out_blanks ++;
    return;
  }

 /*
  * Dither the line into the output buffer...
  */

  y &= 63;
  ditherline = ras->dither[y];

  for (x = ras->left, bit = 128, byte = 0, outptr = ras->out_buffer; x < ras->right; x ++, line ++)
  {
    if (*line <= ditherline[x & 63])
      byte |= bit;

    if (bit == 1)
    {
      *outptr++ = byte;
      byte      = 0;
      bit       = 128;
    }
    else
      bit >>= 1;
  }

  if (bit != 128)
    *outptr++ = byte;

 /*
  * Apply compression...
  */

  compptr = ras->comp_buffer;
  outend  = outptr;
  outptr  = ras->out_buffer;

  while (outptr < outend)
  {
    if ((outptr + 1) >= outend)
    {
     /*
      * Single byte on the end...
      */

      *compptr++ = 0x00;
      *compptr++ = *outptr++;
    }
    else if (outptr[0] == outptr[1])
    {
     /*
      * Repeated sequence...
      */

      outptr ++;
      count = 2;

      while (outptr < (outend - 1) &&
	     outptr[0] == outptr[1] &&
	     count < 127)
      {
	outptr ++;
	count ++;
      }

      *compptr++ = (unsigned char)(257 - count);
      *compptr++ = *outptr++;
    }
    else
    {
     /*
      * Non-repeated sequence...
      */

      start = outptr;
      outptr ++;
      count = 1;

      while (outptr < (outend - 1) &&
	     outptr[0] != outptr[1] &&
	     count < 127)
      {
	outptr ++;
	count ++;
      }

      *compptr++ = (unsigned char)(count - 1);

      memcpy(compptr, start, count);
      compptr += count;
    }
  }

 /*
  * Output the line...
  */

  if (ras->out_blanks > 0)
  {
   /*
    * Skip blank lines first...
    */

    pcl_printf(cb, ctx, "\033*b%dY", ras->out_blanks);
    ras->out_blanks = 0;
  }

  pcl_printf(cb, ctx, "\033*b%dW", (int)(compptr - ras->comp_buffer));
  (*cb)(ctx, ras->comp_buffer, (size_t)(compptr - ras->comp_buffer));
}


/*
 * 'raster_end_job()' - End a raster "job".
 */

static void
raster_end_job(xform_raster_t   *ras,	/* I - Raster information */
	       xform_write_cb_t cb,	/* I - Write callback */
	       void             *ctx)	/* I - Write context */
{
  (void)cb;
  (void)ctx;

  cupsRasterClose(ras->ras);
}


/*
 * 'raster_end_page()' - End of raster page.
 */

static void
raster_end_page(xform_raster_t   *ras,	/* I - Raster information */
	        unsigned         page,	/* I - Current page */
		xform_write_cb_t cb,	/* I - Write callback */
		void             *ctx)	/* I - Write context */
{
  (void)page;
  (void)cb;
  (void)ctx;

  if (ras->header.cupsBitsPerPixel == 1)
  {
    free(ras->out_buffer);
    ras->out_buffer = NULL;
  }
}


/*
 * 'raster_init()' - Initialize callbacks for raster output.
 */

static void
raster_init(xform_raster_t *ras)	/* I - Raster information */
{
  ras->end_job    = raster_end_job;
  ras->end_page   = raster_end_page;
  ras->start_job  = raster_start_job;
  ras->start_page = raster_start_page;
  ras->write_line = raster_write_line;
}


/*
 * 'raster_start_job()' - Start a raster "job".
 */

static void
raster_start_job(xform_raster_t   *ras,	/* I - Raster information */
		 xform_write_cb_t cb,	/* I - Write callback */
		 void             *ctx)	/* I - Write context */
{
  ras->ras = cupsRasterOpenIO((cups_raster_iocb_t)cb, ctx, !strcmp(ras->format, "image/pwg-raster") ? CUPS_RASTER_WRITE_PWG : CUPS_RASTER_WRITE_APPLE);
}


/*
 * 'raster_start_page()' - Start a raster page.
 */

static void
raster_start_page(xform_raster_t   *ras,/* I - Raster information */
		  unsigned         page,/* I - Current page */
		  xform_write_cb_t cb,	/* I - Write callback */
		  void             *ctx)/* I - Write context */
{
  (void)cb;
  (void)ctx;

  ras->left   = 0;
  ras->top    = 0;
  ras->right  = ras->header.cupsWidth;
  ras->bottom = ras->header.cupsHeight;

  if (ras->header.Duplex && !(page & 1))
    cupsRasterWriteHeader2(ras->ras, &ras->back_header);
  else
    cupsRasterWriteHeader2(ras->ras, &ras->header);

  if (ras->header.cupsBitsPerPixel == 1)
  {
    ras->out_length = ras->header.cupsBytesPerLine;
    ras->out_buffer = malloc(ras->header.cupsBytesPerLine);
  }
}


/*
 * 'raster_write_line()' - Write a line of raster data.
 */

static void
raster_write_line(
    xform_raster_t      *ras,		/* I - Raster information */
    unsigned            y,		/* I - Line number */
    const unsigned char *line,		/* I - Pixels on line */
    xform_write_cb_t    cb,		/* I - Write callback */
    void                *ctx)		/* I - Write context */
{
  (void)cb;
  (void)ctx;

  if (ras->header.cupsBitsPerPixel == 1)
  {
   /*
    * Dither the line into the output buffer...
    */

    unsigned		x;		/* Column number */
    unsigned char	bit,		/* Current bit */
			byte,		/* Current byte */
			*outptr;	/* Pointer into output buffer */
    const unsigned char	*ditherline;	/* Pointer into dither table */

    y &= 63;
    ditherline = ras->dither[y];

    if (ras->header.cupsColorSpace == CUPS_CSPACE_SW)
    {
      for (x = ras->left, bit = 128, byte = 0, outptr = ras->out_buffer; x < ras->right; x ++, line ++)
      {
	if (*line > ditherline[x & 63])
	  byte |= bit;

	if (bit == 1)
	{
	  *outptr++ = byte;
	  byte      = 0;
	  bit       = 128;
	}
	else
	  bit >>= 1;
      }
    }
    else
    {
      for (x = ras->left, bit = 128, byte = 0, outptr = ras->out_buffer; x < ras->right; x ++, line ++)
      {
	if (*line <= ditherline[x & 63])
	  byte |= bit;

	if (bit == 1)
	{
	  *outptr++ = byte;
	  byte      = 0;
	  bit       = 128;
	}
	else
	  bit >>= 1;
      }
    }

    if (bit != 128)
      *outptr++ = byte;

    cupsRasterWritePixels(ras->ras, ras->out_buffer, ras->header.cupsBytesPerLine);
  }
  else
    cupsRasterWritePixels(ras->ras, (unsigned char *)line, ras->header.cupsBytesPerLine);
}


/*
 * 'usage()' - Show program usage.
 */

static void
usage(int status)			/* I - Exit status */
{
  puts("Usage: ipptransform [options] filename\n");
  puts("Options:");
  puts("  --help");
  puts("  -d device-uri");
  puts("  -f output-filename");
  puts("  -i input/format");
  puts("  -m output/format");
  puts("  -o \"name=value [... name=value]\"");
  puts("  -r resolution[,...,resolution]");
  puts("  -s {flipped|manual-tumble|normal|rotated}");
  puts("  -t type[,...,type]");
  puts("  -v\n");
  puts("Device URIs: socket://address[:port], ipp://address[:port]/resource, ipps://address[:port]/resource");
  puts("Input Formats: application/pdf, image/jpeg");
  puts("Output Formats: application/vnd.hp-pcl, image/pwg-raster, image/urf");
  puts("Options: copies, media, media-col, page-ranges, print-color-mode, print-quality, print-scaling, printer-resolution, sides");
  puts("Resolutions: NNNdpi or NNNxNNNdpi");
#ifdef HAVE_COREGRAPHICS
  puts("Types: adobe-rgb_8, adobe-rgb_16, black_1, black_8, cmyk_8, sgray_1, sgray_8, srgb_8");
#elif defined(HAVE_FZ_CMM_ENGINE_LCMS)
  puts("Types: adobe-rgb_8, black_1, black_8, cmyk_8, sgray_1, sgray_8, srgb_8");
#else
  puts("Types: black_1, black_8, cmyk_8, sgray_1, sgray_8, srgb_8");
#endif /* HAVE_COREGRAPHICS */

  exit(status);
}


/*
 * 'write_fd()' - Write to a file/socket.
 */

static ssize_t				/* O - Number of bytes written or -1 on error */
write_fd(int                 *fd,	/* I - File descriptor */
         const unsigned char *buffer,	/* I - Buffer */
         size_t              bytes)	/* I - Number of bytes to write */
{
  ssize_t	temp,			/* Temporary byte count */
		total = 0;		/* Total bytes written */


  while (bytes > 0)
  {
    if ((temp = write(*fd, buffer, bytes)) < 0)
    {
      if (errno == EINTR || errno == EAGAIN)
        continue;
      else
        return (-1);
    }

    total  += temp;
    bytes  -= (size_t)temp;
    buffer += temp;
  }

  return (total);
}



#ifdef HAVE_COREGRAPHICS

/*
 * 'xform_document()' - Transform a file for printing.
 */

int				/* O - 0 on success, 1 on error */
xform_document(
    const char       *filename,		/* I - File to transform */
    const char       *informat,		/* I - Input document (MIME media type */
    const char       *outformat,	/* I - Output format (MIME media type) */
    const char       *resolutions,	/* I - Supported resolutions */
    const char       *sheet_back,	/* I - Back side transform */
    const char       *types,		/* I - Supported types */
    int              num_options,	/* I - Number of options */
    cups_option_t    *options,		/* I - Options */
    xform_write_cb_t cb,		/* I - Write callback */
    struct renderer renderer,
    void             *ctx)		/* I - Write context */
{
  CFURLRef		url;		/* CFURL object for PDF filename */
  CGImageSourceRef	src;		/* Image reader */
  CGImageRef		image = NULL;	/* Image */
  xform_raster_t	ras;		/* Raster info */
  size_t		max_raster;	/* Maximum raster memory to use */
  const char		*max_raster_env;/* IPPTRANSFORM_MAX_RASTER env var */
  size_t		bpc;		/* Bits per color */
  CGColorSpaceRef	cs;		/* Quartz color space */
  CGContextRef		context;	/* Quartz bitmap context */
  CGBitmapInfo		info;		/* Bitmap flags */
  size_t		band_size;	/* Size of band line */
  double		xscale, yscale;	/* Scaling factor */
  CGAffineTransform 	transform,	/* Transform for page */
			back_transform;	/* Transform for back side */
  CGRect		dest;		/* Destination rectangle */
  unsigned		pages = 1;	/* Number of pages */
  int			color = 1;	/* Does the PDF have color? */
  const char		*page_ranges;	/* "page-ranges" option */
  unsigned		first = 1,	/* First page of range */
			last = 1;	/* Last page of range */
  const char		*print_scaling;	/* print-scaling option */
  size_t		image_width,	/* Image width */
			image_height;	/* Image height */
  int			image_rotation;	/* Image rotation */
  double		image_xscale,	/* Image scaling */
			image_yscale;
  unsigned		copy;		/* Current copy */
  unsigned		page;		/* Current page */
  unsigned		media_sheets = 0,
			impressions = 0;/* Page/sheet counters */
  void *rendering; /* instance of pdf renderer */

 /*
  * Open the file...
  */

  if ((url = CFURLCreateFromFileSystemRepresentation(kCFAllocatorDefault, (const UInt8 *)filename, (CFIndex)strlen(filename), false)) == NULL)
  {
    fputs("ERROR: Unable to create CFURL for file.\n", stderr);
    return (1);
  }

  if (!strcmp(informat, "application/pdf"))
  {
   /*
    * Open the PDF...
    */
    rendering = renderer.makeRenderer();
    if(!renderer.openDocument(url, rendering)) {
      // open document failed
      renderer.deallocate(rendering);
      return 1;
    }
    CFRelease(url);

   /*
    * Check page ranges...
    */

    if ((page_ranges = cupsGetOption("page-ranges", num_options, options)) != NULL)
    {
      if (sscanf(page_ranges, "%u-%u", &first, &last) != 2 || first > last)
      {
	fprintf(stderr, "ERROR: Bad \"page-ranges\" value '%s'.\n", page_ranges);
	return (1);
      }

      pages = (unsigned)renderer.getPageCount(rendering);
      if (first > pages)
      {
	fputs("ERROR: \"page-ranges\" value does not include any pages to print in the document.\n", stderr);
	renderer.deallocate(rendering);
	return (1);
      }

      if (last > pages)
	last = pages;
    }
    else
    {
      first = 1;
      last  = (unsigned)renderer.getPageCount(rendering);
    }

    pages = last - first + 1;
  }
  else
  {
   /*
    * Open the image...
    */

    if ((src = CGImageSourceCreateWithURL(url, NULL)) == NULL)
    {
      CFRelease(url);
      fputs("ERROR: Unable to create CFImageSourceRef for file.\n", stderr);
      return (1);
    }

    if ((image = CGImageSourceCreateImageAtIndex(src, 0, NULL)) == NULL)
    {
      CFRelease(src);
      CFRelease(url);

      fputs("ERROR: Unable to create CFImageRef for file.\n", stderr);
      return (1);
    }

    CFRelease(src);
    CFRelease(url);

    pages = 1;
  }

 /*
  * Setup the raster context...
  */

  if (xform_setup(&ras, outformat, resolutions, sheet_back, types, color, pages, num_options, options))
  {
    if (image)
      CFRelease(image);

    return (1);
  }

  if (ras.header.cupsBitsPerPixel <= 8)
  {
   /*
    * Grayscale output...
    */

    ras.band_bpp = 1;
    info         = kCGImageAlphaNone;
    cs           = CGColorSpaceCreateWithName(ras.header.cupsColorSpace == CUPS_CSPACE_SW ? kCGColorSpaceGenericGrayGamma2_2 : kCGColorSpaceLinearGray);
    bpc          = 8;
  }
  else if (ras.header.cupsBitsPerPixel == 24)
  {
   /*
    * Color (sRGB or AdobeRGB) output...
    */

    ras.band_bpp = 4;
    info         = kCGImageAlphaNoneSkipLast;
    cs           = CGColorSpaceCreateWithName(ras.header.cupsColorSpace == CUPS_CSPACE_SRGB ? kCGColorSpaceSRGB : kCGColorSpaceAdobeRGB1998);
    bpc          = 8;
  }
  else if (ras.header.cupsBitsPerPixel == 32)
  {
   /*
    * Color (CMYK) output...
    */

    ras.band_bpp = 4;
    info         = kCGImageAlphaNone;
    cs           = CGColorSpaceCreateWithName(kCGColorSpaceGenericCMYK);
    bpc          = 8;
  }
  else
  {
   /*
    * Color (AdobeRGB) output...
    */

    ras.band_bpp = 8;
    info         = kCGImageAlphaNoneSkipLast;
    cs           = CGColorSpaceCreateWithName(kCGColorSpaceAdobeRGB1998);
    bpc          = 16;
  }

  max_raster     = XFORM_MAX_RASTER;
  max_raster_env = getenv("IPPTRANSFORM_MAX_RASTER");
  if (max_raster_env && strtol(max_raster_env, NULL, 10) > 0)
    max_raster = (size_t)strtol(max_raster_env, NULL, 10);

  band_size = ras.header.cupsWidth * ras.band_bpp;
  if ((ras.band_height = (unsigned)(max_raster / band_size)) < 1)
    ras.band_height = 1;
  else if (ras.band_height > ras.header.cupsHeight)
    ras.band_height = ras.header.cupsHeight;

  ras.band_buffer = malloc(ras.band_height * band_size);
  context         = CGBitmapContextCreate(ras.band_buffer, ras.header.cupsWidth, ras.band_height, bpc, band_size, cs, info);

  CGColorSpaceRelease(cs);

  /* Don't anti-alias or interpolate when creating raster data */
  CGContextSetAllowsAntialiasing(context, 0);
  CGContextSetInterpolationQuality(context, kCGInterpolationNone);

  xscale = ras.header.HWResolution[0] / 72.0;
  yscale = ras.header.HWResolution[1] / 72.0;

  if (Verbosity > 1)
    fprintf(stderr, "DEBUG: xscale=%g, yscale=%g\n", xscale, yscale);
  CGContextScaleCTM(context, xscale, yscale);

  if (Verbosity > 1)
    fprintf(stderr, "DEBUG: Band height=%u, page height=%u, page translate 0.0,%g\n", ras.band_height, ras.header.cupsHeight, -1.0 * (ras.header.cupsHeight - ras.band_height) / yscale);
  CGContextTranslateCTM(context, 0.0, -1.0 * (ras.header.cupsHeight - ras.band_height) / yscale);

  dest.origin.x    = dest.origin.y = 0.0;
  dest.size.width  = ras.header.cupsWidth * 72.0 / ras.header.HWResolution[0];
  dest.size.height = ras.header.cupsHeight * 72.0 / ras.header.HWResolution[1];

 /*
  * Get print-scaling value...
  */

  if ((print_scaling = cupsGetOption("print-scaling", num_options, options)) == NULL)
    if ((print_scaling = getenv("IPP_PRINT_SCALING_DEFAULT")) == NULL)
      print_scaling = "auto";

 /*
  * Start the conversion...
  */

  fprintf(stderr, "ATTR: job-impressions=%d\n", pages);
  fprintf(stderr, "ATTR: job-pages=%d\n", pages);

  if (ras.header.Duplex)
    fprintf(stderr, "ATTR: job-media-sheets=%d\n", (pages + 1) / 2);
  else
    fprintf(stderr, "ATTR: job-media-sheets=%d\n", pages);

  if (Verbosity > 1)
    fprintf(stderr, "DEBUG: cupsPageSize=[%g %g]\n", ras.header.cupsPageSize[0], ras.header.cupsPageSize[1]);

  (*(ras.start_job))(&ras, cb, ctx);

  if (rendering)
  {
   /*
    * Render pages in the PDF...
    */

    if (pages > 1 && sheet_back && ras.header.Duplex)
    {
     /*
      * Setup the back page transform...
      */

      if (!strcmp(sheet_back, "flipped"))
      {
	if (ras.header.Tumble)
	  back_transform = CGAffineTransformMake(-1, 0, 0, 1, ras.header.cupsPageSize[0], 0);
	else
	  back_transform = CGAffineTransformMake(1, 0, 0, -1, 0, ras.header.cupsPageSize[1]);
      }
      else if (!strcmp(sheet_back, "manual-tumble") && ras.header.Tumble)
	back_transform = CGAffineTransformMake(-1, 0, 0, -1, ras.header.cupsPageSize[0], ras.header.cupsPageSize[1]);
      else if (!strcmp(sheet_back, "rotated") && !ras.header.Tumble)
	back_transform = CGAffineTransformMake(-1, 0, 0, -1, ras.header.cupsPageSize[0], ras.header.cupsPageSize[1]);
      else
	back_transform = CGAffineTransformMake(1, 0, 0, 1, 0, 0);
    }
    else
      back_transform = CGAffineTransformMake(1, 0, 0, 1, 0, 0);

    if (Verbosity > 1)
      fprintf(stderr, "DEBUG: back_transform=[%g %g %g %g %g %g]\n", back_transform.a, back_transform.b, back_transform.c, back_transform.d, back_transform.tx, back_transform.ty);

   /*
    * Draw all of the pages...
    */

    for (copy = 0; copy < ras.copies; copy ++)
    {
      for (page = 1; page <= pages; page ++)
      {
	unsigned	y,		/* Current line */
			band_starty = 0,/* Start line of band */
			band_endy = 0;	/* End line of band */
	unsigned char	*lineptr;	/* Pointer to line */
	if (!renderer.loadPage(page + first - 1, rendering)) {
	  // Failed loading page
	  renderer.deallocate(rendering);
	  return 1;
	}
    transform = renderer.getPageTransform(rendering);

	if (Verbosity > 1)
	  fprintf(stderr, "DEBUG: Printing copy %d/%d, page %d/%d, transform=[%g %g %g %g %g %g]\n", copy + 1, ras.copies, page, pages, transform.a, transform.b, transform.c, transform.d, transform.tx, transform.ty);

	(*(ras.start_page))(&ras, page, cb, ctx);

	for (y = ras.top; y < ras.bottom; y ++)
	{
	  if (y >= band_endy)
	  {
	   /*
	    * Draw the next band of raster data...
	    */

	    band_starty = y;
	    band_endy   = y + ras.band_height;
	    if (band_endy > ras.bottom)
	      band_endy = ras.bottom;

	    if (Verbosity > 1)
	      fprintf(stderr, "DEBUG: Drawing band from %u to %u.\n", band_starty, band_endy);

	    CGContextSaveGState(context);
	      if (ras.header.cupsNumColors == 1)
		CGContextSetGrayFillColor(context, 1., 1.);
	      else
		CGContextSetRGBFillColor(context, 1., 1., 1., 1.);

	      CGContextSetCTM(context, CGAffineTransformIdentity);
	      CGContextFillRect(context, CGRectMake(0., 0., ras.header.cupsWidth, ras.band_height));
	    CGContextRestoreGState(context);

	    CGContextSaveGState(context);
	      if (Verbosity > 1)
		fprintf(stderr, "DEBUG: Band translate 0.0,%g\n", y / yscale);
	      CGContextTranslateCTM(context, 0.0, y / yscale);
	      if (!(page & 1) && ras.header.Duplex)
		CGContextConcatCTM(context, back_transform);
	      CGContextConcatCTM(context, transform);
          CGContextClipToRect(context, renderer.getPageRect(rendering));
          CGContextSetRGBFillColor(context, 0.8, 0.8, 0.8, 1.);
          if (!renderer.render(context, rendering)) {
            // Rendering failed
            renderer.deallocate(rendering);
            return 1;
          }
	    CGContextRestoreGState(context);
	  }

	 /*
	  * Prepare and write a line...
	  */

	  lineptr = ras.band_buffer + (y - band_starty) * band_size + ras.left * ras.band_bpp;
	  if (ras.header.cupsBitsPerPixel == 24)
	    pack_rgba(lineptr, ras.right - ras.left);
	  else if (ras.header.cupsBitsPerPixel == 48)
	    pack_rgba16(lineptr, ras.right - ras.left);

	  (*(ras.write_line))(&ras, y, lineptr, cb, ctx);
	}

	(*(ras.end_page))(&ras, page, cb, ctx);

	impressions ++;
	fprintf(stderr, "ATTR: job-impressions-completed=%u\n", impressions);
	if (!ras.header.Duplex || !(page & 1))
	{
	  media_sheets ++;
	  fprintf(stderr, "ATTR: job-media-sheets-completed=%u\n", media_sheets);
	}
      }

      if (ras.copies > 1 && (pages & 1) && ras.header.Duplex)
      {
       /*
	* Duplex printing, add a blank back side image...
	*/

	unsigned	y;		/* Current line */

	if (Verbosity > 1)
	  fprintf(stderr, "DEBUG: Printing blank page %u for duplex.\n", pages + 1);

	memset(ras.band_buffer, 255, ras.header.cupsBytesPerLine);

	(*(ras.start_page))(&ras, page, cb, ctx);

	for (y = ras.top; y < ras.bottom; y ++)
	  (*(ras.write_line))(&ras, y, ras.band_buffer, cb, ctx);

	(*(ras.end_page))(&ras, page, cb, ctx);

	impressions ++;
	fprintf(stderr, "ATTR: job-impressions-completed=%u\n", impressions);
	if (!ras.header.Duplex || !(page & 1))
	{
	  media_sheets ++;
	  fprintf(stderr, "ATTR: job-media-sheets-completed=%u\n", media_sheets);
	}
      }
    }
	renderer.deallocate(rendering);
  }
  else
  {
   /*
    * Render copies of the image...
    */

    image_width  = CGImageGetWidth(image);
    image_height = CGImageGetHeight(image);

    if ((image_height < image_width && ras.header.cupsWidth < ras.header.cupsHeight) ||
	 (image_width < image_height && ras.header.cupsHeight < ras.header.cupsWidth))
    {
     /*
      * Rotate image 90 degrees...
      */

      image_rotation = 90;
    }
    else
    {
     /*
      * Leave image as-is...
      */

      image_rotation = 0;
    }

    if (Verbosity > 1)
      fprintf(stderr, "DEBUG: image_width=%u, image_height=%u, image_rotation=%d\n", (unsigned)image_width, (unsigned)image_height, image_rotation);

    if ((!strcmp(print_scaling, "auto") && ras.borderless) || !strcmp(print_scaling, "fill"))
    {
     /*
      * Scale to fill...
      */

      if (image_rotation)
      {
	image_xscale = ras.header.cupsPageSize[0] / (double)image_height;
	image_yscale = ras.header.cupsPageSize[1] / (double)image_width;
      }
      else
      {
	image_xscale = ras.header.cupsPageSize[0] / (double)image_width;
	image_yscale = ras.header.cupsPageSize[1] / (double)image_height;
      }

      if (image_xscale < image_yscale)
	image_xscale = image_yscale;
      else
	image_yscale = image_xscale;

    }
    else
    {
     /*
      * Scale to fit with 1/4" margins...
      */

      if (image_rotation)
      {
	image_xscale = (ras.header.cupsPageSize[0] - 36.0) / (double)image_height;
	image_yscale = (ras.header.cupsPageSize[1] - 36.0) / (double)image_width;
      }
      else
      {
	image_xscale = (ras.header.cupsPageSize[0] - 36.0) / (double)image_width;
	image_yscale = (ras.header.cupsPageSize[1] - 36.0) / (double)image_height;
      }

      if (image_xscale > image_yscale)
	image_xscale = image_yscale;
      else
	image_yscale = image_xscale;
    }

    if (image_rotation)
    {
      transform = CGAffineTransformMake(image_xscale, 0, 0, image_yscale, 0.5 * (ras.header.cupsPageSize[0] - image_xscale * image_height), 0.5 * (ras.header.cupsPageSize[1] - image_yscale * image_width));
    }
    else
    {
      transform = CGAffineTransformMake(image_xscale, 0, 0, image_yscale, 0.5 * (ras.header.cupsPageSize[0] - image_xscale * image_width), 0.5 * (ras.header.cupsPageSize[1] - image_yscale * image_height));
    }

   /*
    * Draw all of the copies...
    */

    for (copy = 0; copy < ras.copies; copy ++)
    {
      unsigned		y,		/* Current line */
			band_starty = 0,/* Start line of band */
			band_endy = 0;	/* End line of band */
      unsigned char	*lineptr;	/* Pointer to line */

      if (Verbosity > 1)
	fprintf(stderr, "DEBUG: Printing copy %d/%d, transform=[%g %g %g %g %g %g]\n", copy + 1, ras.copies, transform.a, transform.b, transform.c, transform.d, transform.tx, transform.ty);

      (*(ras.start_page))(&ras, 1, cb, ctx);

      for (y = ras.top; y < ras.bottom; y ++)
      {
	if (y >= band_endy)
	{
	 /*
	  * Draw the next band of raster data...
	  */

	  band_starty = y;
	  band_endy   = y + ras.band_height;
	  if (band_endy > ras.bottom)
	    band_endy = ras.bottom;

	  if (Verbosity > 1)
	    fprintf(stderr, "DEBUG: Drawing band from %u to %u.\n", band_starty, band_endy);

	  CGContextSaveGState(context);
	    if (ras.header.cupsNumColors == 1)
	      CGContextSetGrayFillColor(context, 1., 1.);
	    else
	      CGContextSetRGBFillColor(context, 1., 1., 1., 1.);

	    CGContextSetCTM(context, CGAffineTransformIdentity);
	    CGContextFillRect(context, CGRectMake(0., 0., ras.header.cupsWidth, ras.band_height));
	  CGContextRestoreGState(context);

	  CGContextSaveGState(context);
	    if (Verbosity > 1)
	      fprintf(stderr, "DEBUG: Band translate 0.0,%g\n", y / yscale);
	    CGContextTranslateCTM(context, 0.0, y / yscale);
	    CGContextConcatCTM(context, transform);

	    if (image_rotation)
	      CGContextConcatCTM(context, CGAffineTransformMake(0, -1, 1, 0, 0, image_width));

	    CGContextDrawImage(context, CGRectMake(0, 0, image_width, image_height), image);
	  CGContextRestoreGState(context);
	}

       /*
	* Prepare and write a line...
	*/

	lineptr = ras.band_buffer + (y - band_starty) * band_size + ras.left * ras.band_bpp;
	if (ras.header.cupsBitsPerPixel == 24)
	  pack_rgba(lineptr, ras.right - ras.left);
	else if (ras.header.cupsBitsPerPixel == 48)
	  pack_rgba16(lineptr, ras.right - ras.left);

	(*(ras.write_line))(&ras, y, lineptr, cb, ctx);
      }

      (*(ras.end_page))(&ras, 1, cb, ctx);

      impressions ++;
      fprintf(stderr, "ATTR: job-impressions-completed=%u\n", impressions);
      media_sheets ++;
      fprintf(stderr, "ATTR: job-media-sheets-completed=%u\n", media_sheets);
    }

    CFRelease(image);
  }

  (*(ras.end_job))(&ras, cb, ctx);

 /*
  * Clean up...
  */

  CGContextRelease(context);

  free(ras.band_buffer);
  ras.band_buffer = NULL;

  return (0);
}


#else
/*
 * 'xform_document()' - Transform a file for printing.
 */

int				/* O - 0 on success, 1 on error */
xform_document(
    const char       *filename,		/* I - File to transform */
    const char       *informat,		/* I - Input format (MIME media type) */
    const char       *outformat,	/* I - Output format (MIME media type) */
    const char       *resolutions,	/* I - Supported resolutions */
    const char       *sheet_back,	/* I - Back side transform */
    const char       *types,		/* I - Supported types */
    int              num_options,	/* I - Number of options */
    cups_option_t    *options,		/* I - Options */
    xform_write_cb_t cb,		/* I - Write callback */
    void             *ctx)		/* I - Write context */
{
  fz_context		*context;	/* MuPDF context */
  fz_document		*document;	/* Document to print */
  fz_page		*pdf_page;	/* Page in PDF file */
  fz_pixmap		*pixmap;	/* Pixmap for band */
  fz_device		*device;	/* Device for rendering */
  fz_colorspace		*cs;		/* Quartz color space */
  xform_raster_t	ras;		/* Raster info */
  size_t		max_raster;	/* Maximum raster memory to use */
  const char		*max_raster_env;/* IPPTRANSFORM_MAX_RASTER env var */
  unsigned		pages = 1;	/* Number of pages */
  int			color = 1;	/* Color PDF? */
  const char		*page_ranges;	/* "page-ranges" option */
  unsigned		first, last;	/* First and last page of range */
  const char		*print_scaling;	/* print-scaling option */
  unsigned		copy;		/* Current copy */
  unsigned		page;		/* Current page */
  unsigned		media_sheets = 0,
			impressions = 0;/* Page/sheet counters */
  size_t		band_size;	/* Size of band line */
  double		xscale, yscale;	/* Scaling factor */
  fz_rect		image_box;	/* Bounding box of content */
  fz_matrix	 	base_transform,	/* Base transform */
			image_transform,/* Transform for content ("page image") */
			transform,	/* Transform for page */
			back_transform;	/* Transform for back side */


 /*
  * Open the PDF file...
  */

  if ((context = fz_new_context(NULL, NULL, FZ_STORE_UNLIMITED)) == NULL)
  {
    fputs("ERROR: Unable to create context.\n", stderr);
    return (1);
  }

  fz_register_document_handlers(context);

  fz_try(context) document = fz_open_document(context, filename);
  fz_catch(context)
  {
    fprintf(stderr, "ERROR: Unable to open '%s': %s\n", filename, fz_caught_message(context));
    fz_drop_context(context);
    return (1);
  }

  if (fz_needs_password(context, document))
  {
    fputs("ERROR: Document is encrypted and cannot be unlocked.\n", stderr);
    fz_drop_document(context, document);
    fz_drop_context(context);
    return (1);
  }

 /*
  * Check page ranges...
  */

  if ((page_ranges = cupsGetOption("page-ranges", num_options, options)) != NULL)
  {
    if (sscanf(page_ranges, "%u-%u", &first, &last) != 2 || first > last)
    {
      fprintf(stderr, "ERROR: Bad \"page-ranges\" value '%s'.\n", page_ranges);

      fz_drop_document(context, document);
      fz_drop_context(context);

      return (1);
    }

    pages = (unsigned)fz_count_pages(context, document);
    if (first > pages)
    {
      fputs("ERROR: \"page-ranges\" value does not include any pages to print in the document.\n", stderr);

      fz_drop_document(context, document);
      fz_drop_context(context);

      return (1);
    }

    if (last > pages)
      last = pages;
  }
  else
  {
    first = 1;
    last  = (unsigned)fz_count_pages(context, document);
  }

  pages = last - first + 1;

 /*
  * Setup the raster context...
  */

  if (xform_setup(&ras, outformat, resolutions, sheet_back, types, color, 1, num_options, options))
  {
    fz_drop_document(context, document);
    fz_drop_context(context);

    return (1);
  }

  if (ras.header.cupsBitsPerPixel <= 8)
  {
   /*
    * Grayscale output...
    */

    ras.band_bpp = 1;
    cs           = fz_device_gray(context);
  }
  else if (ras.header.cupsBitsPerPixel == 24)
  {
   /*
    * Color (sRGB/AdobeRGB) output...
    */

    ras.band_bpp = 3;

#ifdef HAVE_FZ_CMM_ENGINE_LCMS
    if (ras.header.cupsColorSpace == CUPS_CSPACE_ADOBERGB)
    {
      fz_set_cmm_engine(context, &fz_cmm_engine_lcms);

# if 0 /* MuPDF crashes - known bug */
     /*
      * Create a calibrated colorspace using the AdobeRGB (1998) values.
      */

      static float wp_val[] = { 0.9505f, 1.0f, 1.0891f };
      static float bp_val[] = { 0.0f, 0.0f, 0.0f };
      static float gamma_val[] = { 2.19921875f, 2.19921875f, 2.19921875f };
      static float matrix_val[] = {  2.04159f, -0.56501f, -0.34473f,
                                    -0.96924f,  1.87597f,  0.05156f,
                                     0.01344f, -0.11836f,  1.01517f };

      cs = fz_new_cal_colorspace(context, "AdobeRGB", wp_val, bp_val, gamma_val, matrix_val);
#  endif // 0

#  if FZ_VERSION_MAJOR > 1 || FZ_VERSION_MINOR > 14
#    define ADOBE_COLORSPACE	FZ_COLORSPACE_RGB
#  else
#    define ADOBE_COLORSPACE	"AdobeRGB1998"
#  endif /* FZ_VERSION_MAJOR > 1 || FZ_VERSION_MINOR > 14 */

#  ifdef __APPLE__
      cs = fz_new_icc_colorspace_from_file(context, ADOBE_COLORSPACE, "/System/Library/ColorSync/Profiles/AdobeRGB1998.icc");
#  else
      cs = fz_new_icc_colorspace_from_file(context, ADOBE_COLORSPACE, "/usr/share/color/icc/colord/AdobeRGB1998.icc");
#  endif /* __APPLE__ */
    }
    else
#endif /* HAVE_FZ_CMM_ENGINE_LCMS */
    {
     /*
      * Use the "device RGB" colorspace which is sRGB for MuPDF...
      */

      cs = fz_device_rgb(context);
    }
  }
  else if (ras.header.cupsBitsPerPixel == 32)
  {
   /*
    * CMYK output...
    */

    ras.band_bpp = 4;
    cs           = fz_device_cmyk(context);
  }

  max_raster     = XFORM_MAX_RASTER;
  max_raster_env = getenv("IPPTRANSFORM_MAX_RASTER");
  if (max_raster_env && strtol(max_raster_env, NULL, 10) > 0)
    max_raster = (size_t)strtol(max_raster_env, NULL, 10);

  band_size = (size_t)ras.header.cupsWidth * ras.band_bpp;
  fprintf(stderr, "DEBUG: ras.header.cupsWidth=%u, ras.band_bpp=%u, band_size=%ld\n", ras.header.cupsWidth, ras.band_bpp, (long)band_size);

  if ((ras.band_height = (unsigned)(max_raster / band_size)) < 1)
    ras.band_height = 1;
  else if (ras.band_height > ras.header.cupsHeight)
    ras.band_height = ras.header.cupsHeight;

#  if HAVE_FZ_NEW_PIXMAP_5_ARG
  pixmap = fz_new_pixmap(context, cs, (int)ras.header.cupsWidth, (int)ras.band_height, 0);
#  else
  pixmap = fz_new_pixmap(context, cs, (int)ras.header.cupsWidth, (int)ras.band_height, NULL, 0);

  fprintf(stderr, "pixmap->w       = %d\n", pixmap->w);
  fprintf(stderr, "pixmap->h       = %d\n", pixmap->h);
  fprintf(stderr, "pixmap->alpha   = %d\n", pixmap->alpha);
  fprintf(stderr, "pixmap->flags   = %d\n", pixmap->flags);
  fprintf(stderr, "pixmap->xres    = %d\n", pixmap->xres);
  fprintf(stderr, "pixmap->yres    = %d\n", pixmap->yres);
  fprintf(stderr, "pixmap->stride  = %ld\n", (long)pixmap->stride);
  fprintf(stderr, "pixmap->samples = %p\n", pixmap->samples);

  pixmap->flags &= ~FZ_PIXMAP_FLAG_INTERPOLATE;
#  endif /* HAVE_FZ_NEW_PIXMAP_5_ARG */

  pixmap->xres = (int)ras.header.HWResolution[0];
  pixmap->yres = (int)ras.header.HWResolution[1];

  xscale = ras.header.HWResolution[0] / 72.0;
  yscale = ras.header.HWResolution[1] / 72.0;

  if (Verbosity > 1)
    fprintf(stderr, "DEBUG: xscale=%g, yscale=%g\n", xscale, yscale);

#  if FZ_VERSION_MAJOR > 1 || FZ_VERSION_MINOR > 14
  base_transform = fz_scale(xscale, yscale);
#  else
  fz_scale(&base_transform, xscale, yscale);
#  endif /* FZ_VERSION_MAJOR > 1 || FZ_VERSION_MINOR > 14 */

  if (Verbosity > 1)
    fprintf(stderr, "DEBUG: Band height=%u, page height=%u\n", ras.band_height, ras.header.cupsHeight);

#  if FZ_VERSION_MAJOR > 1 || FZ_VERSION_MINOR > 14
  device = fz_new_draw_device(context, base_transform, pixmap);
#  else
  device = fz_new_draw_device(context, &base_transform, pixmap);
#  endif /* FZ_VERSION_MAJOR > 1 || FZ_VERSION_MINOR > 14 */

  /* Don't anti-alias or interpolate when creating raster data */
  fz_set_aa_level(context, 0);
  fz_enable_device_hints(context, device, FZ_DONT_INTERPOLATE_IMAGES);

 /*
  * Setup the back page transform, if any...
  */

  if (sheet_back && ras.header.Duplex)
  {
    if (!strcmp(sheet_back, "flipped"))
    {
      if (ras.header.Tumble)
        back_transform = make_matrix(-1, 0, 0, 1, ras.header.cupsPageSize[0], 0);
      else
        back_transform = make_matrix(1, 0, 0, -1, 0, ras.header.cupsPageSize[1]);
    }
    else if (!strcmp(sheet_back, "manual-tumble") && ras.header.Tumble)
      back_transform = make_matrix(-1, 0, 0, -1, ras.header.cupsPageSize[0], ras.header.cupsPageSize[1]);
    else if (!strcmp(sheet_back, "rotated") && !ras.header.Tumble)
      back_transform = make_matrix(-1, 0, 0, -1, ras.header.cupsPageSize[0], ras.header.cupsPageSize[1]);
    else
      back_transform = make_matrix(1, 0, 0, 1, 0, 0);
  }
  else
    back_transform = make_matrix(1, 0, 0, 1, 0, 0);

  if (Verbosity > 1)
    fprintf(stderr, "DEBUG: cupsPageSize=[%g %g]\n", ras.header.cupsPageSize[0], ras.header.cupsPageSize[1]);
  if (Verbosity > 1)
    fprintf(stderr, "DEBUG: back_transform=[%g %g %g %g %g %g]\n", back_transform.a, back_transform.b, back_transform.c, back_transform.d, back_transform.e, back_transform.f);

 /*
  * Get print-scaling value...
  */

  if ((print_scaling = cupsGetOption("print-scaling", num_options, options)) == NULL)
    if ((print_scaling = getenv("IPP_PRINT_SCALING_DEFAULT")) == NULL)
      print_scaling = "auto";

 /*
  * Draw all of the pages...
  */

  (*(ras.start_job))(&ras, cb, ctx);

  for (copy = 0; copy < ras.copies; copy ++)
  {
    for (page = 1; page <= pages; page ++)
    {
      unsigned		y,		/* Current line */
			band_starty = 0,/* Start line of band */
			band_endy = 0;	/* End line of band */
      unsigned char	*lineptr;	/* Pointer to line */

      pdf_page = fz_load_page(context, document, (int)(page + first - 2));

#  if FZ_VERSION_MAJOR > 1 || FZ_VERSION_MINOR > 14
      image_box = fz_bound_page(context, pdf_page);
#  else
      fz_bound_page(context, pdf_page, &image_box);
#  endif /* FZ_VERSION_MAJOR > 1 || FZ_VERSION_MINOR > 14 */

      fprintf(stderr, "DEBUG: image_box=[%g %g %g %g]\n", image_box.x0, image_box.y0, image_box.x1, image_box.y1);

      float image_width = image_box.x1 - image_box.x0;
      float image_height = image_box.y1 - image_box.y0;
      int image_rotation = 0;
      int is_image = strcmp(informat, "application/pdf") != 0;
      float image_xscale, image_yscale;

      if ((image_height < image_width && ras.header.cupsWidth < ras.header.cupsHeight) ||
	   (image_width < image_height && ras.header.cupsHeight < ras.header.cupsWidth))
      {
       /*
	* Rotate image/page 90 degrees...
	*/

	image_rotation = 90;
      }

      if ((!strcmp(print_scaling, "auto") && ras.borderless && is_image) || !strcmp(print_scaling, "fill"))
      {
       /*
	* Scale to fill...
	*/

	if (image_rotation)
	{
	  image_xscale = ras.header.cupsPageSize[0] / (double)image_height;
	  image_yscale = ras.header.cupsPageSize[1] / (double)image_width;
	}
	else
	{
	  image_xscale = ras.header.cupsPageSize[0] / (double)image_width;
	  image_yscale = ras.header.cupsPageSize[1] / (double)image_height;
	}

	if (image_xscale < image_yscale)
	  image_xscale = image_yscale;
	else
	  image_yscale = image_xscale;

      }
      else if ((!strcmp(print_scaling, "auto") && (is_image || (image_rotation == 0 && (image_width > ras.header.cupsPageSize[0] || image_height > ras.header.cupsPageSize[1])) || (image_rotation == 90 && (image_height > ras.header.cupsPageSize[1] || image_width > ras.header.cupsPageSize[1])))) || !strcmp(print_scaling, "fit"))
      {
       /*
	* Scale to fit...
	*/

	if (image_rotation)
	{
	  image_xscale = ras.header.cupsPageSize[0] / (double)image_height;
	  image_yscale = ras.header.cupsPageSize[1] / (double)image_width;
	}
	else
	{
	  image_xscale = ras.header.cupsPageSize[0] / (double)image_width;
	  image_yscale = ras.header.cupsPageSize[1] / (double)image_height;
	}

	if (image_xscale > image_yscale)
	  image_xscale = image_yscale;
	else
	  image_yscale = image_xscale;
      }
      else
      {
       /*
        * Do not scale...
	*/

        image_xscale = image_yscale = 1.0;
      }

      if (image_rotation)
      {
	image_transform = make_matrix(image_xscale, 0, 0, image_yscale, 0.5 * (ras.header.cupsPageSize[0] - image_xscale * image_height), 0.5 * (ras.header.cupsPageSize[1] - image_yscale * image_width));
      }
      else
      {
	image_transform = make_matrix(image_xscale, 0, 0, image_yscale, 0.5 * (ras.header.cupsPageSize[0] - image_xscale * image_width), 0.5 * (ras.header.cupsPageSize[1] - image_yscale * image_height));
      }

      if (Verbosity > 1)
        fprintf(stderr, "DEBUG: Printing copy %d/%d, page %d/%d, image_transform=[%g %g %g %g %g %g]\n", copy + 1, ras.copies, page, pages, image_transform.a, image_transform.b, image_transform.c, image_transform.d, image_transform.e, image_transform.f);

      (*(ras.start_page))(&ras, page, cb, ctx);

      for (y = ras.top; y < ras.bottom; y ++)
      {
	if (y >= band_endy)
	{
	 /*
	  * Draw the next band of raster data...
	  */

	  band_starty = y;
	  band_endy   = y + ras.band_height;
	  if (band_endy > ras.bottom)
	    band_endy = ras.bottom;

	  if (Verbosity > 1)
	    fprintf(stderr, "DEBUG: Drawing band from %u to %u.\n", band_starty, band_endy);

          fz_clear_pixmap_with_value(context, pixmap, 0xff);
          fputs("DEBUG: Band cleared...\n", stderr);

          transform = fz_identity;

#  if FZ_VERSION_MAJOR > 1 || FZ_VERSION_MINOR > 14
	  transform = fz_pre_translate(transform, 0.0, -1.0 * y / yscale);

	  if (!(page & 1) && ras.header.Duplex)
	    transform = fz_concat(transform, back_transform);

	  transform = fz_concat(transform, image_transform);

#  else
	  fz_pre_translate(&transform, 0.0, -1.0 * y / yscale);

	  if (!(page & 1) && ras.header.Duplex)
	    fz_concat(&transform, &transform, &back_transform);

	  fz_concat(&transform, &transform, &image_transform);
#  endif /* FZ_VERSION_MAJOR > 1 || FZ_VERSION_MINOR > 14 */

          fprintf(stderr, "DEBUG: Page transform=[%g %g %g %g %g %g]\n", transform.a, transform.b, transform.c, transform.d, transform.e, transform.f);

#  if FZ_VERSION_MAJOR > 1 || FZ_VERSION_MINOR > 14
          fz_run_page(context, pdf_page, device, transform, NULL);
#  else
          fz_run_page(context, pdf_page, device, &transform, NULL);
#  endif /* FZ_VERSION_MAJOR > 1 || FZ_VERSION_MINOR > 14 */

          fputs("DEBUG: Band rendered...\n", stderr);
	}

       /*
	* Prepare and write a line...
	*/

	lineptr = pixmap->samples + (y - band_starty) * band_size + ras.left * ras.band_bpp;

        if (ras.header.cupsColorSpace == CUPS_CSPACE_K && ras.header.cupsBitsPerPixel >= 8)
          invert_gray(lineptr, ras.right - ras.left);

	(*(ras.write_line))(&ras, y, lineptr, cb, ctx);
      }

      (*(ras.end_page))(&ras, page, cb, ctx);

      impressions ++;
      fprintf(stderr, "ATTR: job-impressions-completed=%u\n", impressions);
      if (!ras.header.Duplex || !(page & 1))
      {
        media_sheets ++;
	fprintf(stderr, "ATTR: job-media-sheets-completed=%u\n", media_sheets);
      }
    }

    if (ras.copies > 1 && (pages & 1) && ras.header.Duplex)
    {
     /*
      * Duplex printing, add a blank back side image...
      */

      unsigned		y;		/* Current line */

      if (Verbosity > 1)
        fprintf(stderr, "DEBUG: Printing blank page %u for duplex.\n", pages + 1);

      memset(pixmap->samples, ras.header.cupsBitsPerPixel == 32 ? 0 : 255, ras.header.cupsBytesPerLine);

      (*(ras.start_page))(&ras, page, cb, ctx);

      for (y = ras.top; y < ras.bottom; y ++)
	(*(ras.write_line))(&ras, y, pixmap->samples, cb, ctx);

      (*(ras.end_page))(&ras, page, cb, ctx);

      impressions ++;
      fprintf(stderr, "ATTR: job-impressions-completed=%u\n", impressions);
      if (!ras.header.Duplex || !(page & 1))
      {
        media_sheets ++;
	fprintf(stderr, "ATTR: job-media-sheets-completed=%u\n", media_sheets);
      }
    }
  }

  (*(ras.end_job))(&ras, cb, ctx);

 /*
  * Clean up...
  */

  fz_drop_device(context, device);
  fz_drop_pixmap(context, pixmap);
  fz_drop_document(context, document);
  fz_drop_context(context);

  return (1);
}
#endif /* HAVE_COREGRAPHICS */


/*
 * 'xform_setup()' - Setup a raster context for printing.
 */

static int				/* O - 0 on success, -1 on failure */
xform_setup(xform_raster_t *ras,	/* I - Raster information */
            const char     *format,	/* I - Output format (MIME media type) */
	    const char     *resolutions,/* I - Supported resolutions */
	    const char     *sheet_back,	/* I - Back side transform */
	    const char     *types,	/* I - Supported types */
	    int            color,	/* I - Document contains color? */
            unsigned       pages,	/* I - Number of pages */
            int            num_options,	/* I - Number of options */
            cups_option_t  *options)	/* I - Options */
{
  const char	*copies,		/* "copies" option */
		*media,			/* "media" option */
		*media_col;		/* "media-col" option */
  pwg_media_t	*pwg_media = NULL;	/* PWG media value */
  const char	*print_color_mode,	/* "print-color-mode" option */
		*print_quality,		/* "print-quality" option */
		*printer_resolution,	/* "printer-resolution" option */
		*sides,			/* "sides" option */
		*type = NULL;		/* Raster type to use */
  int		pq = IPP_QUALITY_NORMAL,/* Print quality value */
		xdpi, ydpi;		/* Resolution to use */
  cups_array_t	*res_array,		/* Resolutions in array */
		*type_array;		/* Types in array */


 /*
  * Initialize raster information...
  */

  memset(ras, 0, sizeof(xform_raster_t));

  ras->format      = format;
  ras->num_options = num_options;
  ras->options     = options;

  if (!strcmp(format, "application/vnd.hp-pcl"))
    pcl_init(ras);
  else
    raster_init(ras);

 /*
  * Get the number of copies...
  */

  if ((copies = cupsGetOption("copies", num_options, options)) != NULL)
  {
    int temp = atoi(copies);		/* Copies value */

    if (temp < 1 || temp > 9999)
    {
      fprintf(stderr, "ERROR: Invalid \"copies\" value '%s'.\n", copies);
      return (-1);
    }

    ras->copies = (unsigned)temp;
  }
  else
    ras->copies = 1;

 /*
  * Figure out the media size...
  */

  if ((media = cupsGetOption("media", num_options, options)) != NULL)
  {
    if ((pwg_media = pwgMediaForPWG(media)) == NULL)
      pwg_media = pwgMediaForLegacy(media);

    if (!pwg_media)
    {
      fprintf(stderr, "ERROR: Unknown \"media\" value '%s'.\n", media);
      return (-1);
    }
  }
  else if ((media_col = cupsGetOption("media-col", num_options, options)) != NULL)
  {
    int			num_cols;	/* Number of collection values */
    cups_option_t	*cols;		/* Collection values */
    const char		*media_size_name,
			*media_size,	/* Collection attributes */
			*media_bottom_margin,
			*media_left_margin,
			*media_right_margin,
			*media_top_margin;

    num_cols = cupsParseOptions(media_col, 0, &cols);
    if ((media_size_name = cupsGetOption("media-size-name", num_cols, cols)) != NULL)
    {
      if ((pwg_media = pwgMediaForPWG(media_size_name)) == NULL)
      {
	fprintf(stderr, "ERROR: Unknown \"media-size-name\" value '%s'.\n", media_size_name);
	cupsFreeOptions(num_cols, cols);
	return (-1);
      }
    }
    else if ((media_size = cupsGetOption("media-size", num_cols, cols)) != NULL)
    {
      int		num_sizes;	/* Number of collection values */
      cups_option_t	*sizes;		/* Collection values */
      const char	*x_dim,		/* Collection attributes */
			*y_dim;

      num_sizes = cupsParseOptions(media_size, 0, &sizes);
      if ((x_dim = cupsGetOption("x-dimension", num_sizes, sizes)) != NULL && (y_dim = cupsGetOption("y-dimension", num_sizes, sizes)) != NULL)
      {
        pwg_media = pwgMediaForSize(atoi(x_dim), atoi(y_dim));
      }
      else
      {
        fprintf(stderr, "ERROR: Bad \"media-size\" value '%s'.\n", media_size);
	cupsFreeOptions(num_sizes, sizes);
	cupsFreeOptions(num_cols, cols);
	return (-1);
      }

      cupsFreeOptions(num_sizes, sizes);
    }

   /*
    * Check whether the media-col is for a borderless size...
    */

    if ((media_bottom_margin = cupsGetOption("media-bottom-margin", num_cols, cols)) != NULL && !strcmp(media_bottom_margin, "0") &&
        (media_left_margin = cupsGetOption("media-left-margin", num_cols, cols)) != NULL && !strcmp(media_left_margin, "0") &&
        (media_right_margin = cupsGetOption("media-right-margin", num_cols, cols)) != NULL && !strcmp(media_right_margin, "0") &&
        (media_top_margin = cupsGetOption("media-top-margin", num_cols, cols)) != NULL && !strcmp(media_top_margin, "0"))
      ras->borderless = 1;

    cupsFreeOptions(num_cols, cols);
  }

  if (!pwg_media)
  {
   /*
    * Use default size...
    */

    const char	*media_default = getenv("IPP_MEDIA_DEFAULT");
				/* "media-default" value */

    if (!media_default)
      media_default = "na_letter_8.5x11in";

    if ((pwg_media = pwgMediaForPWG(media_default)) == NULL)
    {
      fprintf(stderr, "ERROR: Unknown \"media-default\" value '%s'.\n", media_default);
      return (-1);
    }
  }

 /*
  * Map certain photo sizes (4x6, 5x7, 8x10) to borderless...
  */

  if ((pwg_media->width == 10160 && pwg_media->length == 15240) ||(pwg_media->width == 12700 && pwg_media->length == 17780) ||(pwg_media->width == 20320 && pwg_media->length == 25400))
    ras->borderless = 1;

 /*
  * Figure out the proper resolution, etc.
  */

  res_array = _cupsArrayNewStrings(resolutions, ',');

  if ((printer_resolution = cupsGetOption("printer-resolution", num_options, options)) != NULL && !cupsArrayFind(res_array, (void *)printer_resolution))
  {
    if (Verbosity)
      fprintf(stderr, "INFO: Unsupported \"printer-resolution\" value '%s'.\n", printer_resolution);
    printer_resolution = NULL;
  }

  if (!printer_resolution)
  {
    if ((print_quality = cupsGetOption("print-quality", num_options, options)) != NULL)
    {
      switch (pq = atoi(print_quality))
      {
        case IPP_QUALITY_DRAFT :
	    printer_resolution = cupsArrayIndex(res_array, 0);
	    break;

        case IPP_QUALITY_NORMAL :
	    printer_resolution = cupsArrayIndex(res_array, cupsArrayCount(res_array) / 2);
	    break;

        case IPP_QUALITY_HIGH :
	    printer_resolution = cupsArrayIndex(res_array, cupsArrayCount(res_array) - 1);
	    break;

	default :
	    if (Verbosity)
	      fprintf(stderr, "INFO: Unsupported \"print-quality\" value '%s'.\n", print_quality);
	    break;
      }
    }
  }

  if (!printer_resolution)
    printer_resolution = cupsArrayIndex(res_array, cupsArrayCount(res_array) / 2);

  if (!printer_resolution)
  {
    fputs("ERROR: No \"printer-resolution\" or \"pwg-raster-document-resolution-supported\" value.\n", stderr);
    return (-1);
  }

 /*
  * Parse the "printer-resolution" value...
  */

  if (sscanf(printer_resolution, "%ux%udpi", &xdpi, &ydpi) != 2)
  {
    if (sscanf(printer_resolution, "%udpi", &xdpi) == 1)
    {
      ydpi = xdpi;
    }
    else
    {
      fprintf(stderr, "ERROR: Bad resolution value '%s'.\n", printer_resolution);
      return (-1);
    }
  }

  cupsArrayDelete(res_array);

 /*
  * Now figure out the color space to use...
  */

  if ((print_color_mode = cupsGetOption("print-color-mode", num_options, options)) == NULL)
    print_color_mode = getenv("IPP_PRINT_COLOR_MODE_DEFAULT");

  if (print_color_mode)
  {
    if (!strcmp(print_color_mode, "monochrome") || !strcmp(print_color_mode, "process-monochrome") || !strcmp(print_color_mode, "auto-monochrome"))
    {
      color = 0;
    }
    else if (!strcmp(print_color_mode, "bi-level") || !strcmp(print_color_mode, "process-bi-level"))
    {
      color = 0;
      pq    = IPP_QUALITY_DRAFT;
    }
  }

  if ((type_array = cupsArrayNew3((cups_array_func_t)strcasecmp, NULL, NULL, 0, (cups_acopy_func_t)_cupsStrAlloc, (cups_afree_func_t)_cupsStrFree)) != NULL)
    _cupsArrayAddStrings(type_array, types, ',');

  if (color)
  {
    if (pq == IPP_QUALITY_HIGH)
    {
#ifdef HAVE_COREGRAPHICS
      if (cupsArrayFind(type_array, "adobe-rgb_16"))
	type = "adobe-rgb_16";
      else if (cupsArrayFind(type_array, "adobe-rgb_8"))
	type = "adobe-rgb_8";
#elif defined(HAVE_FZ_CMM_ENGINE_LCMS)
      if (cupsArrayFind(type_array, "adobe-rgb_8"))
	type = "adobe-rgb_8";
#endif /* HAVE_COREGRAPHICS */
    }

    if (!type && cupsArrayFind(type_array, "srgb_8"))
      type = "srgb_8";
    if (!type && cupsArrayFind(type_array, "cmyk_8"))
      type = "cmyk_8";
  }

  if (!type)
  {
    if (pq == IPP_QUALITY_DRAFT)
    {
      if (cupsArrayFind(type_array, "black_1"))
	type = "black_1";
      else if (cupsArrayFind(type_array, "sgray_1"))
	type = "sgray_1";
    }
    else
    {
      if (cupsArrayFind(type_array, "black_8"))
	type = "black_8";
      else if (cupsArrayFind(type_array, "sgray_8"))
	type = "sgray_8";
    }
  }

  if (!type)
  {
   /*
    * No type yet, find any of the supported formats...
    */

    if (cupsArrayFind(type_array, "black_8"))
      type = "black_8";
    else if (cupsArrayFind(type_array, "sgray_8"))
      type = "sgray_8";
    else if (cupsArrayFind(type_array, "black_1"))
      type = "black_1";
    else if (cupsArrayFind(type_array, "sgray_1"))
      type = "sgray_1";
    else if (cupsArrayFind(type_array, "srgb_8"))
      type = "srgb_8";
#ifdef HAVE_COREGRAPHICS
    else if (cupsArrayFind(type_array, "adobe-rgb_8"))
      type = "adobe-rgb_8";
    else if (cupsArrayFind(type_array, "adobe-rgb_16"))
      type = "adobe-rgb_16";
#elif defined(HAVE_FZ_CMM_ENGINE_LCMS)
    else if (cupsArrayFind(type_array, "adobe-rgb_8"))
	type = "adobe-rgb_8";
#endif /* HAVE_COREGRAPHICS */
    else if (cupsArrayFind(type_array, "cmyk_8"))
      type = "cmyk_8";
  }

  cupsArrayDelete(type_array);

  if (!type)
  {
    fputs("ERROR: No supported raster types are available.\n", stderr);
    return (-1);
  }

 /*
  * Initialize the raster header...
  */

  if (pages == 1)
    sides = "one-sided";
  else if ((sides = cupsGetOption("sides", num_options, options)) == NULL)
  {
    if ((sides = getenv("IPP_SIDES_DEFAULT")) == NULL)
      sides = "one-sided";
  }

  if (ras->copies > 1 && (pages & 1) && strcmp(sides, "one-sided"))
    pages ++;

  if (!cupsRasterInitPWGHeader(&(ras->header), pwg_media, type, xdpi, ydpi, sides, NULL))
  {
    fprintf(stderr, "ERROR: Unable to initialize raster context: %s\n", cupsRasterErrorString());
    return (-1);
  }

  if (pages > 1)
  {
    if (!cupsRasterInitPWGHeader(&(ras->back_header), pwg_media, type, xdpi, ydpi, sides, sheet_back))
    {
      fprintf(stderr, "ERROR: Unable to initialize back side raster context: %s\n", cupsRasterErrorString());
      return (-1);
    }
  }

  if (ras->header.cupsBitsPerPixel == 1)
  {
    if (print_color_mode && (!strcmp(print_color_mode, "bi-level") || !strcmp(print_color_mode, "process-bi-level")))
      memset(ras->dither, 127, sizeof(ras->dither));
    else
      memcpy(ras->dither, threshold, sizeof(ras->dither));
  }

  ras->header.cupsInteger[CUPS_RASTER_PWG_TotalPageCount]      = ras->copies * pages;
  ras->back_header.cupsInteger[CUPS_RASTER_PWG_TotalPageCount] = ras->copies * pages;

  if (Verbosity)
  {
    fprintf(stderr, "DEBUG: cupsColorSpace=%u\n", ras->header.cupsColorSpace);
    fprintf(stderr, "DEBUG: cupsBitsPerColor=%u\n", ras->header.cupsBitsPerColor);
    fprintf(stderr, "DEBUG: cupsBitsPerPixel=%u\n", ras->header.cupsBitsPerPixel);
    fprintf(stderr, "DEBUG: cupsNumColors=%u\n", ras->header.cupsNumColors);
    fprintf(stderr, "DEBUG: cupsWidth=%u\n", ras->header.cupsWidth);
    fprintf(stderr, "DEBUG: cupsHeight=%u\n", ras->header.cupsHeight);
  }

  return (0);
}
