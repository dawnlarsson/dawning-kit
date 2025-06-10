// Dawning Documentation Generator v1
#include "../../library.c"
#include "../../platform/shell.c"

#define html_style '<link rel="stylesheet" href="https://dawning.dev/runtime/u&i.css">'
#define tag_line(name, data, content) string_format(log, "<%s %s>%s</%s>", name, data, content, name)
#define tag_empty(name, data) string_format(log, "<%s %s />", name, data)

fn tag(string_address name, string_address data)
{
    if (!string_get(data))
        return string_format(log, "<%s>", name);

    string_format(log, "<%s %s>", name, data);
}

fn html_pop(string_address name)
{
    return string_format(log, "</%s>", name);
}


b32 main()
{
//    dawn_shell_cd(log, "..");

    tag("!DOCTYPE", "html");
    tag("html", "lang=en");
    tag("head", "");
    tag_empty("meta", "charset=utf-8");
    tag_empty("meta", "name=viewport content=\"width=device-width initial-scale=1.0\"");
    tag_empty("meta", "name=description content=\"Dawning C Standard Library Documentation\"");
    html_pop("head");

    tag("body", "");

    //log(lib.data, 30);

    html_pop("body");
    html_pop("html");

    log_flush();
}