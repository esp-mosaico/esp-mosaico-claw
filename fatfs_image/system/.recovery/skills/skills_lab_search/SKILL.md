---
{
  "name": "skills_lab_search",
  "description": "Search skills from the ESP-Skill hub by keywords and advanced filters, then guide the user to download a selected skill with skills_lab_downloader.",
  "metadata": {
    "cap_groups": [
      "cap_http_request",
      "cap_boards"
    ]
  }
}
---

# Skills Lab Search

Use this skill when the user wants to search or discover skills from the ESP-Claw's Skill hub.

## When to use
- The user wants to search for skills from the ESP-Claw's Skill hub.
- The user provides a topic, feature, peripheral, tag, or category and wants matching skill candidates.
- The user does not know the exact skill id needed by `skills_lab_downloader`.

## Hard rules
1. Before searching, make sure `cap_web_search` is enabled and available. If it is unavailable, stop and tell the user to enable web search first.
2. Prefer English keywords for better search results unless the user provides a specific query.
3. Always URL encode the full query string before appending it to `https://skills-lab.esp-claw.com/api/search?q=`.
4. Use the search API endpoint directly. Do not scrape the web UI page for results.
5. If the user asks which tags, peripherals, or categories are available, fetch `https://skills-lab.esp-claw.com/raw/tags.json` and summarize the relevant values.
6. Preserve advanced search syntax exactly before URL encoding: `t:"tag"`, `p:"peripheral"`, and `c:"category"`.
7. By default, add the exact tag filter `t:"esp-mosaico"` to every skill search, including featured and list-all requests. Only omit this filter when the user explicitly says not to limit results by development board or device.
8. By default, only present skills whose returned metadata confirms the `esp-mosaico` tag, and only pass such skills to `skills_lab_downloader`. Do not search for, recommend, or download an untagged skill unless the user explicitly requests no development-board/device restriction.
9. Use `featured=true` when the user wants to filter results to featured skills only.
10. If the user asks to list all available skills, request featured skills with `featured=true` and the default `t:"esp-mosaico"` query, present those featured skills, and ask the user for more specific search criteria. Omit the tag query only under the explicit exception in rule 7.
11. After presenting results, remind the user that installation requires the exact skill id and can be done with `skills_lab_downloader`.

## Search syntax
- Free text keywords: `bilibili fans`
- Tag filter: `t:"info"`
- Peripheral filter: `p:"display"`
- Category filter: `c:"utility"`
- Filters can be combined with free text keywords: `bilibili fans t:"info"`
- A filter-only query is valid: `t:"info"`
- Featured-only filter: `featured=true`

Available `tag`, `peripheral`, and `category` values can be checked at:

```text
https://skills-lab.esp-claw.com/raw/tags.json
```

## Workflow
1. Determine whether the user explicitly requested no development-board/device restriction. Unless they did, append `t:"esp-mosaico"` to the complete search query and enforce the same tag when selecting a result for download.
2. If the user asks to list all available skills, request featured `esp-mosaico` skills by default:

```text
https://skills-lab.esp-claw.com/api/search?q=t%3A%22esp-mosaico%22&featured=true
```

3. Present the featured skill results, then ask the user for more specific search criteria before doing a broader search.
4. Ask for search keywords if the user did not provide any searchable terms, filters, or a request for featured skills.
5. Build a search query with English keywords when possible.
6. Include user-requested or clearly implied advanced filters in addition to the default `t:"esp-mosaico"` filter.
7. Add `featured=true` when the user wants featured skills only.
8. URL encode the complete `q` query value when `q` is used.
9. Request the search API:

```text
https://skills-lab.esp-claw.com/api/search?q=<url_encoded_keywords_and_esp_mosaico_filter>
```

10. When using both `q` and `featured=true`, request:

```text
https://skills-lab.esp-claw.com/api/search?q=<url_encoded_keywords_and_esp_mosaico_filter>&featured=true
```

11. Read the returned search results, verify the `esp-mosaico` tag in each result unless the exception applies, and present the best matches with exact skill ids, titles, and short descriptions when available.
12. If there are no results, suggest broader keywords or fewer optional filters while keeping `t:"esp-mosaico"`; mention removing the board/device restriction only as an explicit opt-in choice.
13. If the user chooses a skill to install, verify that it has the `esp-mosaico` tag unless the exception applies, then activate `skills_lab_downloader` and pass the exact skill id(name).

## Examples
- Query: `bilibili fans`

```text
https://skills-lab.esp-claw.com/api/search?q=bilibili+fans+t%3A%22esp-mosaico%22
```

- Query: `bilibili fans t:"info"`

```text
https://skills-lab.esp-claw.com/api/search?q=bilibili+fans+t%3A%22info%22+t%3A%22esp-mosaico%22
```

- Query: `t:"info"`

```text
https://skills-lab.esp-claw.com/api/search?q=t%3A%22info%22+t%3A%22esp-mosaico%22
```

- Query: `t:"info" quote`

```text
https://skills-lab.esp-claw.com/api/search?q=t%3A%22info%22+quote+t%3A%22esp-mosaico%22
```

- Featured-only query: `bilibili fans`

```text
https://skills-lab.esp-claw.com/api/search?q=bilibili+fans+t%3A%22esp-mosaico%22&featured=true
```

- List featured skills:

```text
https://skills-lab.esp-claw.com/api/search?q=t%3A%22esp-mosaico%22&featured=true
```

- Explicit unrestricted query: `bilibili fans` after the user says not to limit by development board or device

```text
https://skills-lab.esp-claw.com/api/search?q=bilibili+fans
```

## Notes
- Search results are only candidates. Do not install a result until the user selects the exact skill id.
- Do not guess a skill id from a visible title. Use the exact skill id returned by the search API.
- `skills_lab_downloader` handles metadata checks, peripheral compatibility, and installation.
