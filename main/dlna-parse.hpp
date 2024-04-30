#ifndef DLNA_PARSE_H
#define DLNA_PARSE_H
#include <string>
#include <tinyxml2.h>
#include <esp_log.h>

extern const char* kZeroHmsTime;
namespace tinyxml2 {
class XMLElement;
class XMLNode;
}
const tinyxml2::XMLElement* xmlFindPath(const tinyxml2::XMLNode* node, const char* path);
static inline const char* xmlFindPathGetText(const tinyxml2::XMLNode* node, const char* path)
{
    auto elem = xmlFindPath(node, path);
    return elem ? elem->GetText() : nullptr;
}
static inline const char* xmlGetChildText(const tinyxml2::XMLElement& elem, const char* childName, bool logNotFound=true)
{
    auto child = elem.FirstChildElement(childName);
    if (!child) {
        if (logNotFound) {
            ESP_LOGW("DLNA", "%s: Child node %s not found", elem.Name(), childName);
        }
        return nullptr;
    }
    auto result = child->GetText();
    if (!result && logNotFound) {
        ESP_LOGW("DLNA", "%s: Child node %s has no contents", elem.Name(), childName);
    }
    return result;
}

static inline const char* xmlGetChildAttr(const tinyxml2::XMLElement& elem, const char* childName, const char* attrName)
{
    auto child = elem.FirstChildElement(childName);
    if (!child) {
        return nullptr;
    }
    return child->Attribute(attrName);
}

std::string msToHmsString(uint32_t ms);
uint32_t parseHmsTime(const char* hms);


#endif
