#define DEBUG_LOG_TAG "PqXmlParser"
#include "pq_xml_parser.h"

#include <string.h>
#include <stdlib.h>

#include "utils/debug.h"

#define MTK_PQ_XML_PATH_PREFIX "/vendor/etc/"
#define MTK_PQ_XML_PATH_DEFAULT "/vendor/etc/cust_pq.xml"


PqXmlParser::PqXmlParser(std::string panel_name)
    : m_has_xml(false)
    , m_panel_name(panel_name)
{
    init();
}

PqXmlParser::~PqXmlParser()
{
}

void PqXmlParser::init()
{
    HWC_LOGI("%s, panel_name: %s", __func__, m_panel_name.c_str());

    std::string xml_filename;
    if (m_panel_name.compare("") == 0)
    {
        xml_filename = MTK_PQ_XML_PATH_DEFAULT;
        HWC_LOGI("%s: use xml file: %s", __func__, xml_filename.c_str());
    }
    else
    {
        xml_filename = MTK_PQ_XML_PATH_PREFIX + m_panel_name + "_pq.xml";
    }
    // check if file exist, if not fallback to default
    int fd_xml = open(xml_filename.c_str(), O_RDONLY);
    if (fd_xml < 0)
    {
        HWC_LOGW("XML file: %s, is not exist, fallback to default", xml_filename.c_str());
        xml_filename = MTK_PQ_XML_PATH_DEFAULT;
    }
    else
    {
        close(fd_xml);
    }

    HWC_LOGI("%s: use xml file: %s", __func__, xml_filename.c_str());

    xmlDoc* doc = xmlReadFile(xml_filename.c_str(), nullptr, 0);
    if (doc)
    {
        m_has_xml = parseXml(doc);
        xmlFreeDoc(doc);
    }
    else
    {
        HWC_LOGI("%s: failed to open file: %s", __func__, xml_filename.c_str());
        m_has_xml = false;
    }
    xmlCleanupParser();
}

bool PqXmlParser::parseXml(xmlDoc* doc)
{
    bool has_config = false;
    xmlNode *root = xmlDocGetRootElement(doc);
    if (root == nullptr)
    {
        HWC_LOGI("%s: failed to get root", __func__);
    }
    else
    {
        // find PQ_modes
        xmlNode* node_pq_modes = findTargerNode(root, XML_ELEMENT_NODE,
                XML_ELEM_STRING_NODE_PQ_MODES);
        xmlNode* next_mode = nullptr;
        bool has_pq_info = false;
        do {
            // find Mode
            next_mode = findNextTargetNode(node_pq_modes, next_mode, XML_ELEMENT_NODE,
                    XML_ELEM_STRING_NODE_MODE);
            if (next_mode)
            {
                PqModeInfo info;
                bool valid = getPqModeInfo(next_mode, &info);
                if (valid)
                {
                    HWC_LOGI("%s: loading pq mode info: id=%d mode=%d intent=%d range=%d name=%s",
                            __func__, info.id, info.color_mode, info.intent, info.dynamic_range,
                            info.name.c_str());
                    m_color_mode_with_render_intent[info.color_mode].push_back(info);
                    has_pq_info = true;
                }
            }
        } while (next_mode != nullptr);
        if (!has_pq_info)
        {
            HWC_LOGW("%s: can not find pq mode info", __func__);
        }
        else
        {
            has_config = true;
        }
    }
    return has_config;
}

xmlNode* PqXmlParser::findTargerNode(xmlNode* root, xmlElementType type, const char* name)
{
    xmlNode* target = nullptr;
    for (xmlNode* node = root; node != nullptr; node = node->next)
    {
        if (isTargetNode(node, type, name))
        {
            target = node;
            break;
        }
    }

    if (target == nullptr)
    {
        for (xmlNode* node = root; node != nullptr; node = node->next)
        {
            target = findTargerNode(node->children, type, name);
            if (target)
            {
                break;
            }
        }
    }

    return target;
}

xmlNode* PqXmlParser::findNextTargetNode(xmlNode* parent, xmlNode* prev, xmlElementType type,
             const char* name)
{
    if (parent == nullptr)
    {
        return nullptr;
    }

    xmlNode* node = nullptr;
    if (prev == nullptr)
    {
        node = parent->children;
    }
    else
    {
        node = prev->next;
    }

    xmlNode* next = nullptr;
    for (; node != nullptr; node = node->next)
    {
        if (isTargetNode(node, type, name))
        {
            next = node;
            break;
        }
    }

    return next;
}

bool PqXmlParser::getPqModeInfo(xmlNode* node, PqModeInfo* info)
{
    const char* str = nullptr;
    bool res = true;
    // mode name
    str = getTargetAttribute(node, XML_ATTRIBUTE_NODE, XML_ELEM_STRING_ATTR_NAME);
    if (str)
    {
        info->name = str;
    }
    else
    {
        res = false;
    }

    // color mode
    str = getTargetAttribute(node, XML_ATTRIBUTE_NODE, XML_ELEM_STRING_ATTR_COLOR_SPACE);
    if (str)
    {
        size_t size = sizeof(m_color_mode_table) / sizeof(*m_color_mode_table);
        bool find_color_mode = false;
        for (size_t i = 0; i < size; i++)
        {
            if (!strcmp(str, m_color_mode_table[i].second.c_str()))
            {
                info->color_mode = m_color_mode_table[i].first;
                find_color_mode = true;
                break;
            }
        }
        if (!find_color_mode)
        {
            res = false;
        }
    }
    else
    {
        res = false;
    }

    // render intent
    str = getTargetAttribute(node, XML_ATTRIBUTE_NODE, XML_ELEM_STRING_ATTR_RENDER_INTENT);
    if (str)
    {
        info->intent = static_cast<int32_t>(strtol(str, nullptr, 16));
    }
    else
    {
        res = false;
    }

    // dynamic range
    str = getTargetAttribute(node, XML_ATTRIBUTE_NODE, XML_ELEM_STRING_ATTR_DYNAMIC_RANGE);
    if (str)
    {
        bool find_dynamic_range = false;
        for (size_t i = 0; i < XML_ATTR_VALUE_DYNAMIC_RANGE_MAX; i++)
        {
            if (!strcmp(str, m_dynamic_range_table[i].second.c_str()))
            {
                info->dynamic_range = m_dynamic_range_table[i].first;
                find_dynamic_range = true;
                break;
            }
        }
        if (!find_dynamic_range)
        {
            res = false;
        }
    }
    else
    {
        res = false;
    }

    // mode id
    str = getTargetAttribute(node, XML_ATTRIBUTE_NODE, XML_ELEM_STRING_ATTR_MODE_ID);
    if (str)
    {
        info->id = static_cast<int32_t>(strtol(str, nullptr, 10));
    }
    else
    {
        res = false;
    }

    return res;
}

const char* PqXmlParser::getTargetAttribute(xmlNode* node, xmlElementType type, const char* name)
{
    for (xmlAttr* attr = node->properties; attr != nullptr; attr = attr->next)
    {
        if (isTargetAttribute(attr, type, name))
        {
            xmlChar *str = xmlNodeListGetString(node->doc, attr->children, 1);
            return reinterpret_cast<const char*>(str);
        }
    }
    return nullptr;
}

bool PqXmlParser::isTargetNode(xmlNode* node, xmlElementType type, const char* name)
{
    if (node != nullptr && node->type == type &&
            !strcmp(reinterpret_cast<const char*>(node->name), name))
    {
        return true;
    }
    return false;
}

bool PqXmlParser::isTargetAttribute(xmlAttr* attr, xmlElementType type, const char* name)
{
    if (attr != nullptr && attr->type == type &&
            !strcmp(reinterpret_cast<const char*>(attr->name), name))
    {
        return true;
    }
    return false;
}

bool PqXmlParser::hasXml()
{
    return m_has_xml;
}

const std::vector<PqModeInfo>& PqXmlParser::getRenderIntent(int32_t color_mode)
{
    return m_color_mode_with_render_intent[color_mode];
}
