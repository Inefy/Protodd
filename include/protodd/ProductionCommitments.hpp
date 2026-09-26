#pragma once
#include <algorithm>
#include <array>
#include <map>
#include <set>
#include <stdexcept>
#include <vector>

namespace protodd {
// Persistent native execution bookkeeping. No BWAPI command authority. The
// adapter must supply legal offers and reconciled spend/completion observations.
class ProductionCommitments {
public:
    enum class Phase { pending, accepted, uncertain, rejected, failed, completed };
    struct Proposal {
        int key{}, action{}, quantity{}, expires{};
        bool operator==(const Proposal&) const = default;
    };
    struct Offer { int action{}, actor{}, minerals{}, gas{}; bool legal{true}, idle{true}, placement{true}; };
    struct Ticket {
        int key{}, proposal{}, slot{}, actor{}, minerals{}, gas{};
        Phase phase{Phase::pending};
        bool spent{}, actorReleased{}, acceptanceConfirmed{};
        bool terminal() const { return phase==Phase::rejected||phase==Phase::failed||phase==Phase::completed; }
        bool holdsResources() const { return !spent&&!terminal(); }
        bool holdsActor() const { return !actorReleased&&!terminal(); }
    };
    struct Snapshot {
        int frame{}, minerals{}, gas{};
        std::set<int> live, spent, completed, failed, started;
        bool operator==(const Snapshot&) const = default;
    };
    explicit ProductionCommitments(std::size_t limit=1024):limit_(limit) {
        require(limit>0,"positive ticket limit required");
    }
    bool submit(Proposal p) {
        require(p.key>=0&&p.action>=0&&p.action<8&&p.quantity>=1&&p.quantity<=4&&p.expires>frame_,"invalid proposal");
        if(proposals_.contains(p.key)) {require(proposals_.at(p.key)==p,"immutable proposal changed");return false;}
        if(!enabled_||p.key<=lastProposal_)return false;
        if(proposals_.size()>=limit_){disable();return false;}
        for(const auto& [key,prior]:proposals_)require(prior.action!=p.action||!active(prior),"type already owned");
        proposals_[p.key]=p;lastProposal_=p.key;return true;
    }
    void cancel(int key){require(proposals_.contains(key),"unknown proposal");cancelled_.insert(key);}
    void disable(){enabled_=false;for(const auto& [key,p]:proposals_)cancelled_.insert(key);}
    void accepted(int key) {
        auto& t=tickets_.at(key);if(t.phase==Phase::accepted)return;
        require(t.phase==Phase::pending||t.phase==Phase::uncertain,"acceptance conflicts");
        t.phase=Phase::accepted;t.acceptanceConfirmed=true;
    }
    void rejected(int key) {
        auto& t=tickets_.at(key);if(t.phase==Phase::rejected)return;
        require((t.phase==Phase::pending||t.phase==Phase::uncertain)&&!t.acceptanceConfirmed,"rejection conflicts");
        t.phase=Phase::rejected;t.actorReleased=true;
    }
    void observe(const Snapshot& s) {
        require(s.frame>=frame_&&s.frame>=0&&s.minerals>=0&&s.gas>=0,"invalid snapshot");
        if(s.frame==frame_){require(s==snapshot_,"same frame changed");return;}
        std::set<int> feedback=s.spent;
        for(const auto* values:{&s.completed,&s.failed,&s.started})feedback.insert(values->begin(),values->end());
        // Validate everything before changing the bank, tickets or chronology.
        for(int key:feedback) {
            require(tickets_.contains(key),"unknown feedback ticket");const auto& t=tickets_.at(key);
            require(t.phase==Phase::accepted||t.phase==Phase::uncertain||t.phase==Phase::completed||t.phase==Phase::failed,"feedback before acceptance");
            require(!(s.completed.contains(key)&&s.failed.contains(key)),"conflicting outcomes");
            require(!(s.completed.contains(key)&&t.phase==Phase::failed)&&!(s.failed.contains(key)&&t.phase==Phase::completed),"terminal outcome changed");
            if(s.completed.contains(key)||s.failed.contains(key)||s.started.contains(key))
                require(t.spent||s.spent.contains(key),"outcome without spending reconciliation");
        }
        snapshot_=s;frame_=s.frame;
        for(int key:s.spent)tickets_.at(key).spent=true;
        // Product ownership persists through completion; the actor can serve a
        // subsequent legal, idle offer after a product has been bound and paid.
        for(int key:s.started)tickets_.at(key).actorReleased=true;
        for(int key:s.completed){auto& t=tickets_.at(key);t.phase=Phase::completed;t.actorReleased=true;}
        for(int key:s.failed){auto& t=tickets_.at(key);t.phase=Phase::failed;t.actorReleased=true;}
        for(auto& [key,t]:tickets_)if(t.holdsActor()&&!s.live.contains(t.actor)) {
            if(t.spent){t.phase=Phase::failed;t.actorReleased=true;}else t.phase=Phase::uncertain;
        }
    }
    std::array<int,2> reservations() const {
        std::array<int,2> total{};
        for(const auto& [key,t]:tickets_)if(t.holdsResources()){total[0]+=t.minerals;total[1]+=t.gas;}
        return total;
    }
    std::vector<int> reserve(const std::vector<Offer>& offers) {
        if(!enabled_||frame_<0)return {};
        for(const auto& o:offers)require(o.action>=0&&o.action<8&&o.actor>=0&&o.minerals>=0&&o.gas>=0,"invalid offer");
        const auto held=reservations();int m=snapshot_.minerals-held[0],g=snapshot_.gas-held[1];
        std::set<int> busy;for(const auto& [key,t]:tickets_)if(t.holdsActor())busy.insert(t.actor);
        std::vector<int> result;
        for(const auto& [key,p]:proposals_)if(active(p))for(int slot=0;slot<p.quantity;++slot) {
            int attempts=0;bool occupied=false;
            for(const auto& [tk,t]:tickets_)if(t.proposal==key&&t.slot==slot){++attempts;occupied|=t.phase!=Phase::rejected&&t.phase!=Phase::failed;}
            if(attempts>=2||occupied)continue;
            if(tickets_.size()>=limit_){disable();return result;}
            const auto found=std::find_if(offers.begin(),offers.end(),[&](const Offer& o){return o.action==p.action&&
                snapshot_.live.contains(o.actor)&&!busy.contains(o.actor)&&o.legal&&o.idle&&o.placement&&o.minerals<=m&&o.gas<=g;});
            if(found==offers.end())continue;
            const auto& o=*found;const int ticket=static_cast<int>(tickets_.size())+1;
            tickets_[ticket]={ticket,key,slot,o.actor,o.minerals,o.gas};result.push_back(ticket);
            busy.insert(o.actor);m-=o.minerals;g-=o.gas;
        }
        return result;
    }
    const Ticket& ticket(int key) const {return tickets_.at(key);}
    bool enabled() const{return enabled_;}
private:
    static void require(bool ok,const char* message){if(!ok)throw std::invalid_argument(message);}
    bool active(const Proposal& p) const{return !cancelled_.contains(p.key)&&p.expires>frame_;}
    std::size_t limit_;
    bool enabled_{true};int frame_{-1},lastProposal_{-1};
    Snapshot snapshot_;
    std::map<int,Proposal> proposals_;
    std::map<int,Ticket> tickets_;
    std::set<int> cancelled_;
};
}
