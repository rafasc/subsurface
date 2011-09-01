#include <stdio.h>
#include <ctype.h>
#include <string.h>
#include <stdlib.h>
#include <errno.h>
#include <time.h>
#include <libxml/parser.h>
#include <libxml/tree.h>

#include "dive.h"

int verbose;

struct dive_table dive_table;

/*
 * Add a dive into the dive_table array
 */
static void record_dive(struct dive *dive)
{
	int nr = dive_table.nr, allocated = dive_table.allocated;
	struct dive **dives = dive_table.dives;

	if (nr >= allocated) {
		allocated = (nr + 32) * 3 / 2;
		dives = realloc(dives, allocated * sizeof(struct dive *));
		if (!dives)
			exit(1);
		dive_table.dives = dives;
		dive_table.allocated = allocated;
	}
	dives[nr] = dive;
	dive_table.nr = nr+1;
}

static void start_match(const char *type, const char *name, char *buffer)
{
	if (verbose > 2)
		printf("Matching %s '%s' (%s)\n",
			type, name, buffer);
}

static void nonmatch(const char *type, const char *name, char *buffer)
{
	if (verbose > 1)
		printf("Unable to match %s '%s' (%s)\n",
			type, name, buffer);
	free(buffer);
}

typedef void (*matchfn_t)(char *buffer, void *);

static int match(const char *pattern, int plen,
		 const char *name, int nlen,
		 matchfn_t fn, char *buf, void *data)
{
	if (plen > nlen)
		return 0;
	if (memcmp(pattern, name + nlen - plen, plen))
		return 0;
	fn(buf, data);
	return 1;
}

/*
 * Dive info as it is being built up..
 */
static int alloc_samples;
static struct dive *dive;
static struct sample *sample;
static struct tm tm;
static int suunto;
static int event_index, gasmix_index;

static time_t utc_mktime(struct tm *tm)
{
	static const int mdays[] = {
	    0, 31, 59, 90, 120, 151, 181, 212, 243, 273, 304, 334
	};
	int year = tm->tm_year;
	int month = tm->tm_mon;
	int day = tm->tm_mday;

	/* First normalize relative to 1900 */
	if (year < 70)
		year += 100;
	else if (year > 1900)
		year -= 1900;

	/* Normalized to Jan 1, 1970: unix time */
	year -= 70;

	if (year < 0 || year > 129) /* algo only works for 1970-2099 */
		return -1;
	if (month < 0 || month > 11) /* array bounds */
		return -1;
	if (month < 2 || (year + 2) % 4)
		day--;
	if (tm->tm_hour < 0 || tm->tm_min < 0 || tm->tm_sec < 0)
		return -1;
	return (year * 365 + (year + 1) / 4 + mdays[month] + day) * 24*60*60UL +
		tm->tm_hour * 60*60 + tm->tm_min * 60 + tm->tm_sec;
}

static void divedate(char *buffer, void *_when)
{
	int d,m,y;
	time_t *when = _when;

	if (sscanf(buffer, "%d.%d.%d", &d, &m, &y) == 3) {
		tm.tm_year = y;
		tm.tm_mon = m-1;
		tm.tm_mday = d;
		if (tm.tm_sec | tm.tm_min | tm.tm_hour)
			*when = utc_mktime(&tm);
	}
	free(buffer);
}

static void divetime(char *buffer, void *_when)
{
	int h,m,s = 0;
	time_t *when = _when;

	if (sscanf(buffer, "%d:%d:%d", &h, &m, &s) >= 2) {
		tm.tm_hour = h;
		tm.tm_min = m;
		tm.tm_sec = s;
		if (tm.tm_year)
			*when = utc_mktime(&tm);
	}
	free(buffer);
}

/* Libdivecomputer: "2011-03-20 10:22:38" */
static void divedatetime(char *buffer, void *_when)
{
	int y,m,d;
	int hr,min,sec;
	time_t *when = _when;

	if (sscanf(buffer, "%d-%d-%d %d:%d:%d",
		&y, &m, &d, &hr, &min, &sec) == 6) {
		tm.tm_year = y;
		tm.tm_mon = m-1;
		tm.tm_mday = d;
		tm.tm_hour = hr;
		tm.tm_min = min;
		tm.tm_sec = sec;
		*when = utc_mktime(&tm);
	}
	free(buffer);
}

union int_or_float {
	long i;
	double fp;
};

enum number_type {
	NEITHER,
	INTEGER,
	FLOAT
};

static enum number_type integer_or_float(char *buffer, union int_or_float *res)
{
	char *end;
	long val;
	double fp;

	/* Integer or floating point? */
	val = strtol(buffer, &end, 10);
	if (val < 0 || end == buffer)
		return NEITHER;

	/* Looks like it might be floating point? */
	if (*end == '.') {
		errno = 0;
		fp = strtod(buffer, &end);
		if (!errno) {
			res->fp = fp;
			return FLOAT;
		}
	}

	res->i = val;
	return INTEGER;
}

static void pressure(char *buffer, void *_press)
{
	pressure_t *pressure = _press;
	union int_or_float val;

	switch (integer_or_float(buffer, &val)) {
	case FLOAT:
		/* Maybe it's in Bar? */
		if (val.fp < 500.0) {
			pressure->mbar = val.fp * 1000;
			break;
		}
		printf("Unknown fractional pressure reading %s\n", buffer);
		break;

	case INTEGER:
		/*
		 * Random integer? Maybe in PSI? Or millibar already?
		 *
		 * We assume that 5 bar is a ridiculous tank pressure,
		 * so if it's smaller than 5000, it's in PSI..
		 */
		if (val.i < 5000) {
			pressure->mbar = val.i * 68.95;
			break;
		}
		pressure->mbar = val.i;
		break;
	default:
		printf("Strange pressure reading %s\n", buffer);
	}
	free(buffer);
}

static void depth(char *buffer, void *_depth)
{
	depth_t *depth = _depth;
	union int_or_float val;

	switch (integer_or_float(buffer, &val)) {
	/* All values are probably in meters */
	case INTEGER:
		val.fp = val.i;
		/* fallthrough */
	case FLOAT:
		depth->mm = val.fp * 1000;
		break;
	default:
		printf("Strange depth reading %s\n", buffer);
	}
	free(buffer);
}

static void temperature(char *buffer, void *_temperature)
{
	temperature_t *temperature = _temperature;
	union int_or_float val;

	switch (integer_or_float(buffer, &val)) {
	/* C or F? Who knows? Let's default to Celsius */
	case INTEGER:
		val.fp = val.i;
		/* Fallthrough */
	case FLOAT:
		/* Ignore zero. It means "none" */
		if (!val.fp)
			break;
		/* Celsius */
		if (val.fp < 50.0) {
			temperature->mkelvin = (val.fp + 273.16) * 1000;
			break;
		}
		/* Fahrenheit */
		if (val.fp < 212.0) {
			temperature->mkelvin = (val.fp + 459.67) * 5000/9;
			break;
		}
		/* Kelvin or already millikelvin */
		if (val.fp < 1000.0)
			val.fp *= 1000;
		temperature->mkelvin = val.fp;
		break;
	default:
		printf("Strange temperature reading %s\n", buffer);
	}
	free(buffer);
}

static void sampletime(char *buffer, void *_time)
{
	int i;
	int min, sec;
	duration_t *time = _time;

	i = sscanf(buffer, "%d:%d", &min, &sec);
	switch (i) {
	case 1:
		sec = min;
		min = 0;
	/* fallthrough */
	case 2:
		time->seconds = sec + min*60;
		break;
	default:
		printf("Strange sample time reading %s\n", buffer);
	}
	free(buffer);
}

static void duration(char *buffer, void *_time)
{
	sampletime(buffer, _time);
}

static void percent(char *buffer, void *_fraction)
{
	fraction_t *fraction = _fraction;
	union int_or_float val;

	switch (integer_or_float(buffer, &val)) {
	/* C or F? Who knows? Let's default to Celsius */
	case INTEGER:
		val.fp = val.i;
		/* Fallthrough */
	case FLOAT:
		if (val.fp <= 100.0)
			fraction->permille = val.fp * 10 + 0.5;
		break;

	default:
		printf("Strange percentage reading %s\n", buffer);
		break;
	}
	free(buffer);
}

static void gasmix(char *buffer, void *_fraction)
{
	/* libdivecomputer does negative percentages. */
	if (*buffer == '-')
		return;
	if (gasmix_index < MAX_MIXES)
		percent(buffer, _fraction);
}

static void gasmix_nitrogen(char *buffer, void *_gasmix)
{
	/* Ignore n2 percentages. There's no value in them. */
}

#define MATCH(pattern, fn, dest) \
	match(pattern, strlen(pattern), name, len, fn, buf, dest)

/* We're in samples - try to convert the random xml value to something useful */
static void try_to_fill_sample(struct sample *sample, const char *name, char *buf)
{
	int len = strlen(name);

	start_match("sample", name, buf);
	if (MATCH(".sample.pressure", pressure, &sample->tankpressure))
		return;
	if (MATCH(".sample.cylpress", pressure, &sample->tankpressure))
		return;
	if (MATCH(".sample.depth", depth, &sample->depth))
		return;
	if (MATCH(".sample.temperature", temperature, &sample->temperature))
		return;
	if (MATCH(".sample.sampletime", sampletime, &sample->time))
		return;
	if (MATCH(".sample.time", sampletime, &sample->time))
		return;

	nonmatch("sample", name, buf);
}

/*
 * Crazy suunto xml. Look at how those o2/he things match up.
 */
static int suunto_dive_match(struct dive *dive, const char *name, int len, char *buf)
{
	return	MATCH(".o2pct", percent, &dive->gasmix[0].o2) ||
		MATCH(".hepct_0", percent, &dive->gasmix[0].he) ||
		MATCH(".o2pct_2", percent, &dive->gasmix[1].o2) ||
		MATCH(".hepct_1", percent, &dive->gasmix[1].he) ||
		MATCH(".o2pct_3", percent, &dive->gasmix[2].o2) ||
		MATCH(".hepct_2", percent, &dive->gasmix[2].he) ||
		MATCH(".o2pct_4", percent, &dive->gasmix[3].o2) ||
		MATCH(".hepct_3", percent, &dive->gasmix[3].he);
}

/* We're in the top-level dive xml. Try to convert whatever value to a dive value */
static void try_to_fill_dive(struct dive *dive, const char *name, char *buf)
{
	int len = strlen(name);

	start_match("dive", name, buf);
	if (MATCH(".date", divedate, &dive->when))
		return;
	if (MATCH(".time", divetime, &dive->when))
		return;
	if (MATCH(".datetime", divedatetime, &dive->when))
		return;
	if (MATCH(".maxdepth", depth, &dive->maxdepth))
		return;
	if (MATCH(".meandepth", depth, &dive->meandepth))
		return;
	if (MATCH(".divetime", duration, &dive->duration))
		return;
	if (MATCH(".divetimesec", duration, &dive->duration))
		return;
	if (MATCH(".surfacetime", duration, &dive->surfacetime))
		return;
	if (MATCH(".airtemp", temperature, &dive->airtemp))
		return;
	if (MATCH(".watertemp", temperature, &dive->watertemp))
		return;
	if (MATCH(".cylinderstartpressure", pressure, &dive->beginning_pressure))
		return;
	if (MATCH(".cylinderendpressure", pressure, &dive->end_pressure))
		return;

	if (MATCH(".o2", gasmix, &dive->gasmix[gasmix_index].o2))
		return;
	if (MATCH(".n2", gasmix_nitrogen, &dive->gasmix[gasmix_index]))
		return;
	if (MATCH(".he", gasmix, &dive->gasmix[gasmix_index].he))
		return;

	/* Suunto XML files are some crazy sh*t. */
	if (suunto && suunto_dive_match(dive, name, len, buf))
		return;

	nonmatch("dive", name, buf);
}

static unsigned int dive_size(int samples)
{
	return sizeof(struct dive) + samples*sizeof(struct sample);
}

/*
 * File boundaries are dive boundaries. But sometimes there are
 * multiple dives per file, so there can be other events too that
 * trigger a "new dive" marker and you may get some nesting due
 * to that. Just ignore nesting levels.
 */
static void dive_start(void)
{
	unsigned int size;

	if (dive)
		return;

	alloc_samples = 5;
	size = dive_size(alloc_samples);
	dive = malloc(size);
	if (!dive)
		exit(1);
	memset(dive, 0, size);
	memset(&tm, 0, sizeof(tm));
}

static char *generate_name(struct dive *dive)
{
	int len;
	struct tm *tm;
	char buffer[256], *p;

	tm = gmtime(&dive->when);

	len = snprintf(buffer, sizeof(buffer),
		"%04d-%02d-%02d "
		"%02d:%02d:%02d "
		"(%d ft, %d min)",
		tm->tm_year+1900, tm->tm_mon+1, tm->tm_mday,
		tm->tm_hour, tm->tm_min, tm->tm_sec,
		to_feet(dive->maxdepth), dive->duration.seconds / 60);
	p = malloc(len+1);
	if (!p)
		exit(1);
	memcpy(p, buffer, len+1);
	return p;
}

static void sanitize_gasmix(struct dive *dive)
{
	int i;

	for (i = 0; i < MAX_MIXES; i++) {
		gasmix_t *mix = dive->gasmix+i;
		unsigned int o2, he;

		o2 = mix->o2.permille;
		he = mix->he.permille;

		/* Regular air: leave empty */
		if (!he) {
			if (!o2)
				continue;
			/* 20.9% or 21% O2 is just air */
			if (o2 >= 209 && o2 <= 210) {
				mix->o2.permille = 0;
				continue;
			}
		}

		/* Sane mix? */
		if (o2 <= 1000 && he <= 1000 && o2+he <= 1000)
			continue;
		fprintf(stderr, "Odd gasmix: %d O2 %d He\n", o2, he);
		memset(mix, 0, sizeof(*mix));
	}
}

static void dive_end(void)
{
	if (!dive)
		return;
	if (!dive->name)
		dive->name = generate_name(dive);
	sanitize_gasmix(dive);
	record_dive(dive);
	dive = NULL;
	gasmix_index = 0;
}

static void suunto_start(void)
{
	suunto++;
}

static void suunto_end(void)
{
	suunto--;
}

static void event_start(void)
{
}

static void event_end(void)
{
	event_index++;
}

static void gasmix_start(void)
{
}

static void gasmix_end(void)
{
	gasmix_index++;
}

static void sample_start(void)
{
	int nr;

	if (!dive)
		return;
	nr = dive->samples;
	if (nr >= alloc_samples) {
		unsigned int size;

		alloc_samples = (alloc_samples * 3)/2 + 10;
		size = dive_size(alloc_samples);
		dive = realloc(dive, size);
		if (!dive)
			return;
	}
	sample = dive->sample + nr;
	memset(sample, 0, sizeof(*sample));
	event_index = 0;
}

static void sample_end(void)
{
	if (!dive)
		return;

	if (sample->time.seconds > dive->duration.seconds) {
		if (sample->depth.mm)
			dive->duration = sample->time;
	}

	if (sample->depth.mm > dive->maxdepth.mm)
		dive->maxdepth.mm = sample->depth.mm;

	sample = NULL;
	dive->samples++;
}

static void entry(const char *name, int size, const char *raw)
{
	char *buf = malloc(size+1);

	if (!buf)
		return;
	memcpy(buf, raw, size);
	buf[size] = 0;
	if (sample) {
		try_to_fill_sample(sample, name, buf);
		return;
	}
	if (dive) {
		try_to_fill_dive(dive, name, buf);
		return;
	}
}

static const char *nodename(xmlNode *node, char *buf, int len)
{
	if (!node || !node->name)
		return "root";

	buf += len;
	*--buf = 0;
	len--;

	for(;;) {
		const char *name = node->name;
		int i = strlen(name);
		while (--i >= 0) {
			unsigned char c = name[i];
			*--buf = tolower(c);
			if (!--len)
				return buf;
		}
		node = node->parent;
		if (!node || !node->name)
			return buf;
		*--buf = '.';
		if (!--len)
			return buf;
	}
}

#define MAXNAME 64

static void visit_one_node(xmlNode *node)
{
	int len;
	const unsigned char *content;
	char buffer[MAXNAME];
	const char *name;

	content = node->content;
	if (!content)
		return;

	/* Trim whitespace at beginning */
	while (isspace(*content))
		content++;

	/* Trim whitespace at end */
	len = strlen(content);
	while (len && isspace(content[len-1]))
		len--;

	if (!len)
		return;

	/* Don't print out the node name if it is "text" */
	if (!strcmp(node->name, "text"))
		node = node->parent;

	name = nodename(node, buffer, sizeof(buffer));

	entry(name, len, content);
}

static void traverse(xmlNode *root);

static void traverse_properties(xmlNode *node)
{
	xmlAttr *p;

	for (p = node->properties; p; p = p->next)
		traverse(p->children);
}

static void visit(xmlNode *n)
{
	visit_one_node(n);
	traverse_properties(n);
	traverse(n->children);
}

/*
 * I'm sure this could be done as some fancy DTD rules.
 * It's just not worth the headache.
 */
static struct nesting {
	const char *name;
	void (*start)(void), (*end)(void);
} nesting[] = {
	{ "dive", dive_start, dive_end },
	{ "SUUNTO", suunto_start, suunto_end },
	{ "sample", sample_start, sample_end },
	{ "SAMPLE", sample_start, sample_end },
	{ "event", event_start, event_end },
	{ "gasmix", gasmix_start, gasmix_end },
	{ NULL, }
};

static void traverse(xmlNode *root)
{
	xmlNode *n;

	for (n = root; n; n = n->next) {
		struct nesting *rule = nesting;

		do {
			if (!strcmp(rule->name, n->name))
				break;
			rule++;
		} while (rule->name);

		if (rule->start)
			rule->start();
		visit(n);
		if (rule->end)
			rule->end();
	}
}

void parse_xml_file(const char *filename)
{
	xmlDoc *doc;

	doc = xmlReadFile(filename, NULL, 0);
	if (!doc) {
		fprintf(stderr, "Failed to parse '%s'.\n", filename);
		return;
	}

	dive_start();
	traverse(xmlDocGetRootElement(doc));
	dive_end();
	xmlFreeDoc(doc);
	xmlCleanupParser();
}

void parse_xml_init(void)
{
	LIBXML_TEST_VERSION
}